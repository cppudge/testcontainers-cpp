# testcontainers-cpp

[![CI](https://github.com/cppudge/testcontainers-cpp/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/cppudge/testcontainers-cpp/actions/workflows/ci.yml)

Native C++ port of [Testcontainers](https://testcontainers.com/) — spin up real Docker
containers from your integration tests and tear them down automatically. **No Rust, no `docker`
CLI**: a pure C++ client that talks to the Docker Engine HTTP API directly.

> **Status:** alpha, feature-complete core. `GenericImage` / `GenericBuildableImage` /
> `Container` / `Network` / `Volume` / `DockerComposeContainer`, six wait strategies, exec
> (stdin / tty / streaming), copy to/from container, lifecycle hooks, container reuse, the Ryuk
> crash-safety reaper, registry auth incl. credential helpers, host-port exposure
> (`with_exposed_host_port`, sshd sidecar + SSH tunnel), and the TLS transport are
> implemented, covered by ~300 unit + ~115 integration tests against a real daemon — green on
> Windows (named pipe) and Linux (unix socket; CI runs the full suite in BOTH engine modes:
> the Windows job drives real Windows containers — build/volumes/networks/exec/copy/ports/
> waits/lifecycle each have a Windows mirror suite). CI also gates on `-Wall -Wextra`//W4
> as errors, pinned clang-format + clang-tidy, an ASan+UBSan run of both test suites, and
> CodeQL. The feature reference
> with known limits: [`docs/06`](docs/06-feature-notes.md). End-to-end TLS against a real
> remote daemon is the main unverified gap.

This is a from-scratch rewrite. A previous attempt (`testcontainers-cxx/`, in this repo for
reference) wrapped the Rust `testcontainers` library over a cxx FFI bridge; this project drops
the bridge and reimplements everything natively in idiomatic C++.

## Why

- Run integration tests against real services (databases, brokers, …) the same way you run unit
  tests — each test owns an isolated, disposable environment.
- A single C++ dependency, no Rust toolchain, no shelling out to `docker`.
- First-class Windows support (Docker Desktop named pipe), Linux/macOS (unix socket), and remote
  Docker (TCP+TLS).

## How it works

Testcontainers is, at its core, a **client over the Docker Engine HTTP API**. The flow:

1. Resolve where the Docker daemon listens (env vars → socket/pipe → defaults).
2. `POST /containers/create` → `POST /containers/{id}/start`.
3. **Wait for readiness** (log message, HTTP probe, healthcheck, exit).
4. `GET /containers/{id}/json` → discover the **host port** Docker published.
5. Hand the user a handle to talk to the container.
6. On scope exit, `DELETE /containers/{id}` (RAII), with a Ryuk reaper as crash-safety net.

Transport matrix (all HTTP/1.1, over a single Boost.Asio stream abstraction):

| Platform | Docker endpoint | Asio stream |
|---|---|---|
| Linux / macOS | unix socket `/var/run/docker.sock` | `local::stream_protocol::socket` |
| Windows | named pipe `//./pipe/docker_engine` | `windows::stream_handle` |
| Remote / CI | `tcp://` / `https://` | `tcp::socket` / `ssl::stream` |

Full architecture analysis of the reference Rust implementation: [`docs/01`](docs/01-how-testcontainers-rs-works.md).

## Usage

Include the umbrella header `testcontainers/testcontainers.hpp` (or the individual headers) and link
the `testcontainers::testcontainers` CMake target. Everything lives in `namespace testcontainers`.

### Quick start — a service in a test

```cpp
#include <gtest/gtest.h>
#include "testcontainers/testcontainers.hpp"

using namespace testcontainers;

TEST(Cache, RedisIsReachable) {
    // Start Redis, block until it logs readiness, then talk to it.
    Container redis = GenericImage("redis", "7.2")
                          .with_exposed_port(tcp(6379))
                          .with_wait(wait_for::stdout_message("Ready to accept connections"))
                          .start();

    const std::string   host = redis.host();                  // "localhost" for a local daemon
    const std::uint16_t port = redis.get_host_port(tcp(6379)); // the published host port

    // ... point your Redis client at host:port and run assertions ...

}   // `redis` is force-removed here (RAII). If the process is *killed* instead,
    // the Ryuk reaper removes it a few seconds later.
```

### Configuring the image

`GenericImage` is a copyable, reusable builder; each `with_*` mutates in place and chains:

```cpp
auto postgres = GenericImage("postgres", "16-alpine")
                    .with_env("POSTGRES_PASSWORD", "secret")
                    .with_env("POSTGRES_DB", "test")
                    .with_exposed_port(tcp(5432))
                    .with_wait(wait_for::stdout_message("ready to accept connections", 2))
                    .with_startup_timeout(std::chrono::seconds(30));

Container a = postgres.start();   // reusable — start as many as you need
Container b = postgres.start();
```

Builders: `with_cmd`, `with_entrypoint`, `with_env`, `with_label`, `with_exposed_port`,
`with_working_dir`, `with_user`, `with_privileged`, `with_mount` (`Mount::bind/volume/tmpfs`),
`with_healthcheck` (`Healthcheck::cmd_shell/cmd`), `with_copy_to`, `with_network`,
`with_network_alias`, `with_static_ipv4`, `with_container_name`, `with_registry_auth`,
`with_wait`, `with_startup_timeout`. Statics: `from_reference("name[:tag]")` and
`exists("name[:tag]")` — a local-presence probe, e.g. to skip an expensive
`GenericBuildableImage::build()` (which can stream its output live via
`with_build_log_consumer`) when a previous run already built the image.

Wait strategies — `with_wait(wait_for::…)`: `stdout_message` / `stderr_message` / `log`,
`seconds` / `millis`, `exit` / `exit_code`, `healthy` (Docker healthcheck), `http(path, port, status)`.
Several may be chained; they run in order under the startup timeout.

### Talking to the running container

`Container` is a move-only RAII handle: `host()`, `get_host_port(port)`, `logs()`, `exec(cmd)`,
`copy_to(src)`, `stop()`, `is_running()`, `remove()`.

```cpp
ExecResult r = redis.exec({"redis-cli", "PING"});  // r.stdout_data / r.stderr_data / r.exit_code
ContainerLogs l = redis.logs();                     // l.stdout_data / l.stderr_data
redis.copy_to(CopyToContainer::content("seed-data", "/tmp/seed.txt"));
```

### Several containers on one network

Put containers on a user-defined network so they resolve each other by container name:

```cpp
Network net = Network::create();

Container redis = GenericImage("redis", "7.2")
                      .with_network(net.name())
                      .with_container_name("redis")          // peers reach it as "redis"
                      .with_wait(wait_for::stdout_message("Ready to accept connections"))
                      .start();

Container app = GenericImage("my/app", "latest")
                    .with_network(net.name())
                    .with_env("REDIS_URL", "redis://redis:6379")
                    .start();
```

### Cleanup

Containers and networks are removed when their handle goes out of scope. As a crash-safety net, a
[Ryuk](https://github.com/testcontainers/moby-ryuk) sidecar reaps everything tagged with the run's
session label if the process dies (e.g. `SIGKILL`, where destructors never run). Opt out with
`TESTCONTAINERS_RYUK_DISABLED=true`. Runnable examples live in [`tests/integration/`](tests/integration).

## Tech stack

Dependencies are managed by **Conan 2** (public ConanCenter) and wired into CMake via
[`cmake-conan`](https://github.com/conan-io/cmake-conan) (vendored at `cmake/cmake-conan/`).

| Package | Version | Role |
|---|---|---|
| `boost` (header-only: Beast + Asio) | 1.91.0 | HTTP/1.1 + transport (unix socket / **Windows named pipe** / TCP+TLS), streaming & connection hijack |
| `openssl` | 3.6.3 | TLS for `https://`/`tcp+tls` Docker hosts |
| `nlohmann_json` | 3.12.0 | request/response bodies, `inspect`/`Ports`/`Health` parsing |
| `libarchive` | 3.8.7 | tar for copy-to/from container and build context |

Rationale for choosing Boost.Beast+Asio over libcurl/cpr/cpp-httplib (named-pipe support was the
deciding factor): [`docs/02`](docs/02-dependencies.md).

## Design principles (native C++, not a Rust transliteration)

Lessons from auditing the FFI fork ([`docs/03`](docs/03-cxx-interface-evaluation.md)):

- **Value types are plain, copyable structs/enums** (`ContainerPort`, `Mount`, `WaitFor`, …).
  No move-only-on-data, no opaque handles.
- **Builders mutate in place and chain** via a single `with_*` overload returning `T&` — one
  unqualified overload chains on both named lvalues and temporaries. No "consume-self-return-Self"
  — named configs are reusable, no use-after-move footguns. (Move-only handles like
  `DockerComposeContainer` keep a `&`/`&&` pair so a chained temporary can move-construct.)
- **No trait-mirror interfaces.** Runtime polymorphism only where there's real extension:
  a user-facing `Image` base (custom images) and optionally `IWaitStrategy`.
- **`WaitFor` is a `std::variant`** of small structs, matched with `std::visit`.
- **RAII only where a real resource is owned** — the running `Container` (auto-removed on
  destruction); everything else is a value.
- **Structured errors** (exception hierarchy carrying HTTP status / container id), since we own
  the HTTP layer.
- **Connection-per-request, no global pool** — the correctness-first choice the Rust reference
  (bollard) also makes; the Java reference's shared unvalidated pool is the source of its
  stale-connection and fd-leak issues. Hot polling loops opt into scoped keep-alive reuse via
  `DockerClient::Session` (idempotent requests only, retry-once on a stale connection).

## Project layout

```
include/testcontainers/      public API (GenericImage, Container, Network, value types)
  docker/                    DockerClient + low-level types (DockerHost, ContainerSpec, Logs)
src/  src/docker/            implementation: transport, endpoints, tar, auth, log demux, Ryuk
examples/                    tc_smoke (links all deps), tc_ping (GET /_ping)
tests/unit/  tests/integration/   GoogleTest — unit (no Docker) + integration (real daemon)
cmake/cmake-conan/           vendored cmake-conan provider
docs/                        design + feature docs (01–06) + conventions
TODO.md                      actionable backlog (implemented features + limits: docs/06)
_research/testcontainers-rs/ cloned Rust reference sources (gitignored)
testcontainers-cxx/          previous FFI-bridge fork (reference only, gitignored)
```

## Roadmap

- [x] Architecture analysis of testcontainers-rs
- [x] Dependency selection (Conan 2 / Boost.Beast)
- [x] Native interface design (audit of the FFI fork)
- [x] **CMake + Conan project skeleton** (smoke build links all deps)
- [x] **`DockerClient`**: transport abstraction (unix socket / Windows named pipe / TCP) +
      Docker host resolution + `GET /_ping` (verified against Docker Desktop over named pipe).
      _TLS (https) transport still pending._
- [x] **Core container endpoints**: pull / create (+ lazy pull on 404) / start / inspect /
      stop / remove, with `ApiMapping` (types ↔ Docker JSON). Covered by 21 unit tests +
      3 integration tests (real container lifecycle, skipped if no daemon).
- [x] **Multiplexed log-stream parser**; container logs + `GET /containers/{id}/logs`
      (incremental demuxer handling split headers/payloads, `DockerClient::logs()`).
- [x] Value types (`ContainerPort`, `WaitFor`) & `GenericImage`/`Container` builder; `start()` lifecycle
- [x] Host-port discovery from `NetworkSettings.Ports` (`Container::get_host_port`)
- [x] Wait strategies: log message, fixed duration, **exit (optional code), healthcheck (`State.Health`),
      and HTTP probe (host-port GET)**, run sequentially under a shared 60s startup timeout
- [x] **MVP**: `GenericImage("redis","7.2")` up → connect → auto-remove
- [x] **Richer container config**: entrypoint, working dir, user, privileged, and a `Mount`
      value type (bind / volume / tmpfs) mapped into the create body + `HostConfig`
- [x] **`exec`** (run a command in a running container, capturing stdout/stderr + exit code)
      and **user-defined networks** (`Network` RAII handle; `GenericImage::with_network` /
      `with_container_name` so peers resolve each other by name)
- [x] **Ryuk resource reaper** (crash-safety net): a process-wide session id +
      `org.testcontainers.{managed-by,session-id}` labels on every created container/network;
      a single `testcontainers/ryuk:0.11.0` sidecar (docker.sock bind-mounted, 8080/tcp published)
      holds a persistent TCP control connection and reaps everything matching the session label
      when the process dies (covers `SIGKILL`/crash where C++ destructors don't run). Opt out via
      `TESTCONTAINERS_RYUK_DISABLED=true`.
- [x] **Registry authentication** (pull private images): `X-Registry-Auth` on
      `POST /images/create`, sent for both explicit credentials
      (`GenericImage::with_registry_auth` / `DockerClient::pull_image(image, auth)`)
      and auto-resolved ones from the Docker config (`DOCKER_AUTH_CONFIG` →
      `$DOCKER_CONFIG/config.json` → `~/.docker/config.json`). Registry resolution
      follows the Docker CLI heuristic (`ghcr.io/...`→`ghcr.io`,
      `confluentinc/cp-kafka`→`index.docker.io`), with a small standalone base64
      codec. Credential helpers (`credsStore`/`credHelpers`) are not yet
      supported (no shelling out). Covered by 17 unit tests.
- [x] **Copy files/data into containers**: a `CopyToContainer` value type
      (`host_file` / `content` / recursive `host_dir` + `with_mode`) PUT as a tar
      to `PUT /containers/{id}/archive?path=/` (libarchive USTAR, built
      in-memory; Windows drive-rooted targets like `C:\dir\x` are normalized).
      Supported both at creation (`GenericImage::with_copy_to`, applied
      create→copy→start so a copy failure removes the container) and into a
      running container (`Container::copy_to`).
- [x] **Windows-container support** (parity with testcontainers-dotnet): daemon-OS
      detection (`DockerClient::server_os()` / `is_windows_engine()` via `GET /version`,
      cached process-wide), a free-form create `platform` (`GenericImage::with_platform`,
      e.g. `windows/amd64` → `?platform=`), and **skipping Ryuk on the Windows engine**
      (the Linux Ryuk image cannot run there — so there is **no crash-safe reaping on
      Windows**; cleanup falls back to RAII removal + `AutoRemove`). Integration tests are
      engine-aware: Linux-image tests skip in Windows-containers mode and vice versa (see
      `tests/integration/EngineGuard.hpp`), and every cross-engine feature has a Windows
      mirror suite (`WindowsBuildImage` / `WindowsVolumes` / `WindowsNetworks` / `WindowsExec`
      / `WindowsCopy` / `WindowsPortGetters` / `WindowsWaitStrategies` / `WindowsLifecycle` +
      the `WindowsContainer` smoke suite) running real `mcr.microsoft.com/windows/nanoserver`
      (and, for the in-container TCP listener, `servercore`) containers end to end.
      `with_isolation` ("process"/"hyperv") maps to `HostConfig.Isolation` for Windows
      daemons. Covered by 7 unit tests + 34 Windows-mode integration tests. Engine-mode
      coverage matrix: [`docs/07`](docs/07-public-api-test-coverage.md).
- [x] **Everything since** — transport I/O deadlines + structured errors, TLS transport, full
      Docker host resolution, follow/streaming logs, exec stdin/tty/streaming, TTY containers,
      lifecycle hooks + startup retry, build-from-Dockerfile, image pull policy + name
      substitution, credential helpers, container reuse, named volumes, richer networks,
      Docker Compose (three client modes), the `ContainerRequest`/`Runner` split, scoped
      keep-alive sessions, and host-port exposure (`with_exposed_host_port`:
      `testcontainers/sshd` sidecar + libssh2 remote forwards, so containers reach host
      services at `host.testcontainers.internal:<port>`). Current state and known limits per
      feature: [`docs/06-feature-notes.md`](docs/06-feature-notes.md).

## Build

Requires CMake ≥ 3.21, a C++20 compiler, Conan ≥ 2, and Ninja. `cmake-conan` (vendored) runs
`conan install` automatically during configure and auto-detects a profile — no manual
`conan profile detect` needed.

By default the build uses a **project-local Conan home** at `./.conan2` (the project root),
isolated from your global Conan home and seeded with only the public ConanCenter remote. Set
`-DTC_LOCAL_CONAN_HOME=OFF` to use the global home instead (then ensure ConanCenter is reachable
there: `conan remote add conancenter https://center2.conan.io`).

Presets live in `CMakePresets.json`:

| Preset | Generator | Config | Build dir |
|---|---|---|---|
| `ninja` | Ninja | Release | `build/ninja` |
| `ninja-debug` | Ninja | Debug | `build/ninja-debug` |
| `msvc` | Visual Studio 2022 | Release | `build/msvc` |

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --preset ninja                 # ctest --preset ninja-unit  → unit tests only
./build/ninja/bin/tc_smoke           # tc_smoke / tc_ping (.exe on Windows)
```

On Windows, Ninja needs the MSVC toolchain on `PATH`: run the commands from an
**"x64 Native Tools Command Prompt for VS 2022"**, or use VS Code (below), which applies the
developer environment for you. The `ninja-debug` preset's first configure builds the Debug
dependencies (e.g. OpenSSL) from source.

### Open in VS Code

1. Install the recommended extensions (VS Code offers them from `.vscode/extensions.json`):
   **CMake Tools** and **clangd**.
2. Open the folder. CMake Tools reads `CMakePresets.json` — pick the **`ninja`** configure preset
   from the status bar and configure. That runs Conan, generates the compile database, and applies
   the VS developer environment automatically.
3. clangd reads `build/ninja/compile_commands.json` (via the `.clangd` file) for accurate
   IntelliSense and diagnostics. Build / run tests from the CMake Tools status bar.

Style and static analysis are enforced in CI: `.clang-format` (pinned wheel —
`pip install clang-format==22.1.5`, run over `*.cpp *.hpp`) and `.clang-tidy`
(`pip install clang-tidy==18.1.8`, run with `-p build/ninja`; clangd applies the same
config in-editor as you type).

> Note: `libarchive` is built with `with_iconv=False` — its `libiconv` dependency has no prebuilt
> Windows/msvc-194 binary and fails to build from source (rc.exe flag mismatch); we don't need
> iconv for tar.

## License

Licensed under either of

- MIT license ([LICENSE-MIT](LICENSE-MIT))
- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE))

at your option.

Unless you explicitly state otherwise, any contribution intentionally submitted for inclusion
in this work by you, as defined in the Apache-2.0 license, shall be dual licensed as above,
without any additional terms or conditions.
