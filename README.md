# testcontainers-cpp

[![CI](https://github.com/cppudge/testcontainers-cpp/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/cppudge/testcontainers-cpp/actions/workflows/ci.yml)

Native C++20 port of [Testcontainers](https://testcontainers.com/) — spin up real Docker
containers from your integration tests and tear them down automatically. **No `docker` CLI
required**: a single C++ library speaking the Docker Engine HTTP API directly, with full
streaming control over pulls, logs, and exec.

Run your tests against the real thing — Postgres, Redis, a broker, your own image — instead of a
mock. Each test owns an isolated, disposable environment, created on `start()` and force-removed
when its handle leaves scope (RAII), with a crash-safety reaper as backstop. First-class on Linux
(unix socket), Windows (Docker Desktop named pipe, including real **Windows containers**), and
remote daemons (TCP+TLS).

## Quick start

Include the umbrella header `testcontainers/testcontainers.hpp`, link the
`testcontainers::testcontainers` target, and everything lives in `namespace testcontainers`:

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

    const std::string   host = redis.host();                   // "localhost" for a local daemon
    const std::uint16_t port = redis.get_host_port(tcp(6379)); // the published host port

    // ... point your Redis client at host:port and run assertions ...

}   // `redis` is force-removed here (RAII). If the process is *killed* instead,
    // the Ryuk reaper removes it a few seconds later.
```

`GenericImage` is a copyable, reusable builder; each `with_*` mutates in place and chains.
`Container` is a move-only RAII handle: `host()`, `get_host_port()`, `logs()`, `exec()`,
`copy_to()`, `inspect()`, `stop()`, `remove()`. Errors are thrown, not returned — a `DockerError`
exception hierarchy carrying the HTTP status and resource id. Runnable end-to-end examples live in
[`tests/integration/`](tests/integration).

## Installation

testcontainers-cpp is a **Conan 2** package. A ConanCenter submission is staged for right after the
`v0.1.0` tag (see [Packaging](#packaging)); until it lands, build the package from this repo into
your local Conan cache:

```sh
git clone https://github.com/cppudge/testcontainers-cpp
conan create testcontainers-cpp --build=missing -s compiler.cppstd=20
```

Then require it from your project's `conanfile.txt` (the package reference is `testcontainers-cpp`):

```ini
[requires]
testcontainers-cpp/0.1.0

[generators]
CMakeDeps
CMakeToolchain
```

and wire it into CMake with `find_package` (the CMake package and target are named `testcontainers`):

```cmake
find_package(testcontainers REQUIRED)
target_link_libraries(my_tests PRIVATE testcontainers::testcontainers)
```

A plain `cmake --install` of this repo works too, exporting the same
`find_package(testcontainers)` / `testcontainers::testcontainers` contract for non-Conan consumers.

Two build options (both default **ON**) let consumers who must avoid a transitive OpenSSL trim the
graph: conan `tls` / CMake `TC_TLS` gates the `https://` transport, and conan `host_port_forwarding`
/ CMake `TC_HOST_PORT_FORWARDING` gates `with_exposed_host_port`'s sshd sidecar (libssh2). Disabling
**both** removes OpenSSL from the dependency graph entirely; a disabled path throws a `DockerError`
naming the option.

## Requirements

- **C++20** — the public headers require it (they will not parse under C++17).
- **Docker** reachable from the test host (local daemon, Docker Desktop, or a remote engine).
- **CMake ≥ 3.21**, **Conan ≥ 2**, and **Ninja** to build from source.

Supported compilers (enforced by the Conan recipe's `validate()`):

| Compiler | Minimum |
|---|---|
| GCC | 12 |
| Clang | 15 |
| MSVC | 19.3 (Visual Studio 2022) |
| Apple Clang | 15 |

Platform support — CI builds and runs the full suite against a real daemon in **both** engine
modes:

| Platform | Docker endpoint | CI coverage |
|---|---|---|
| Linux | unix socket | full unit + integration suite (gcc) |
| Windows | named pipe | full suite against **real Windows containers** (MSVC) |
| macOS | unix socket (Docker Desktop) | `conan create` + unit suite (apple-clang) |
| Remote | `tcp://` / `https://` | TLS unit-tested; end-to-end against a remote daemon not yet verified |

## Status

**v0.1.0 — first release; feature-complete core, pre-1.0.** The public API is settled enough to use
but may still evolve before 1.0. The library is exception-based by design (the `DockerError`
hierarchy). Implemented and covered by **~300 unit + ~115 integration** tests against a real daemon,
green on Windows (named pipe) and Linux (unix socket):

- `GenericImage` / `GenericBuildableImage` / `Container` / `Network` / `Volume` /
  `DockerComposeContainer`
- six wait strategies, `exec` (stdin / tty / streaming), copy to/from container, lifecycle hooks
- container reuse, the Ryuk crash-safety reaper, registry auth incl. credential helpers
- host-port exposure (`with_exposed_host_port` — sshd sidecar + SSH tunnel), and the TLS transport

Known gaps: **end-to-end TLS against a real remote daemon** is the main unverified path (the pure
`TlsConfig` resolution is unit-tested); shared builds are **static-only on Windows** (the
sources carry no symbol-export macros yet); and the Ryuk reaper and the host-port-forwarding
sidecar are **process-global** — they bind to the first daemon used, so a process talking to a
second daemon gets labels but no crash-safe reaping there. Per-feature limits are tracked in
[`docs/feature-notes.md`](docs/feature-notes.md) and [`docs/TODO.md`](docs/TODO.md).

---

## How it works

Testcontainers is, at its core, a **client over the Docker Engine HTTP API**. The flow:

1. Resolve where the Docker daemon listens (env vars → socket/pipe → defaults).
2. `POST /containers/create` → `POST /containers/{id}/start`.
3. **Wait for readiness** (log message, HTTP probe, healthcheck, exit).
4. `GET /containers/{id}/json` → discover the **host port** Docker published.
5. Hand the user a handle to talk to the container.
6. On scope exit, `DELETE /containers/{id}` (RAII), with a Ryuk reaper as crash-safety net.

Every transport is HTTP/1.1 over a single Boost.Asio stream abstraction (`ITransport`), chosen by
the resolved endpoint's scheme:

| Platform | Docker endpoint | Asio stream |
|---|---|---|
| Linux / macOS | unix socket `/var/run/docker.sock` | `local::stream_protocol::socket` |
| Windows | named pipe `//./pipe/docker_engine` | `windows::stream_handle` |
| Remote / CI | `tcp://` / `https://` | `tcp::socket` / `ssl::stream` |

Boost.Beast+Asio was chosen over libcurl/cpr/cpp-httplib because Windows named-pipe support was
the deciding factor. The client is connection-per-request by default (the correctness-first choice
the Rust reference also makes), with hot polling loops opting into scoped keep-alive reuse.

## Features

- **Images** — `GenericImage` builder (env, cmd, entrypoint, labels, mounts, user, privileged,
  healthcheck, static IPv4, copy-to, registry auth) and `GenericBuildableImage` (build from a
  Dockerfile + context, live build-log streaming, local-presence `exists`/`inspect` probes).
- **Wait strategies** — log message, fixed duration, exit code, Docker healthcheck, HTTP probe,
  listening port; chained under one startup timeout.
- **Containers** — typed/raw `inspect`, `logs` (snapshot + streaming follow), `exec` (env / cwd /
  user / tty / stdin / detached / streaming), copy to/from, IPv4/IPv6 host-port getters.
- **Networking & volumes** — user-defined `Network` (driver, IPAM, aliases) and named `Volume`
  handles, both RAII and session-labeled.
- **Lifecycle** — created/starting/started/stopping hooks, startup retry, container `reuse`
  (find-or-create by config hash), and the [Ryuk](https://github.com/testcontainers/moby-ryuk)
  reaper for crash-safe cleanup (Linux engines).
- **Host access & Compose** — `with_exposed_host_port` (a `testcontainers/sshd` sidecar tunnel back
  to the test host) and `DockerComposeContainer` (local CLI / containerised / auto client modes).
- **Windows containers** — engine-mode detection, `with_platform` / `with_isolation`, and a mirror
  integration suite (build / volumes / networks / exec / copy / ports / waits / lifecycle).

Per-feature reference with known limits: [`docs/feature-notes.md`](docs/feature-notes.md).
Public-API coverage in each engine mode: [`docs/public-api-test-coverage.md`](docs/public-api-test-coverage.md).

## Development

Dependencies are managed by Conan 2 (public ConanCenter) and wired into CMake via the vendored
[`cmake-conan`](https://github.com/conan-io/cmake-conan) provider, which runs `conan install`
automatically during configure — no manual `conan profile detect` needed. By default the build uses
a project-local Conan home at `./.conan2` (set `-DTC_LOCAL_CONAN_HOME=OFF` for the global one).

Core dependencies: `boost` 1.91.0 (header-only Beast + Asio), `nlohmann_json` 3.12.0, `libarchive`
3.8.7, plus `openssl` 3.6.3 and `libssh2` 1.11.1 for the optional TLS / host-port features.

Presets live in `CMakePresets.json`:

| Preset | Generator | Config |
|---|---|---|
| `ninja` | Ninja | Release |
| `ninja-debug` | Ninja | Debug |
| `msvc` | Visual Studio 2022 | Release |

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --preset ninja                 # ctest --preset ninja-unit  → unit tests only
./build/ninja/bin/tc_smoke           # tc_smoke / tc_ping examples (.exe on Windows)
```

On Windows, Ninja needs the MSVC toolchain on `PATH` (run from an *"x64 Native Tools Command Prompt
for VS 2022"*, or use VS Code with the CMake Tools + clangd extensions, which apply the developer
environment for you).

Two test suites: **unit** (`tc_unit_tests`, no Docker) and **integration** (`tc_integration_tests`,
requires a reachable daemon; skips gracefully otherwise). CI gates every push on:

- `-Wall -Wextra` (gcc/clang) / `/W4` (MSVC) **as errors** (`TC_WERROR`).
- Pinned **clang-format** (22.1.5) and **clang-tidy** (18.1.8) — `.clang-format` / `.clang-tidy` are
  the single source of truth (clangd applies the same in-editor).
- An **ASan + UBSan** run of both suites.
- **CodeQL** semantic analysis, whose traced build also doubles as the clang compile+link gate.
- A minimal-features Linux build (`TC_TLS=OFF` + `TC_HOST_PORT_FORWARDING=OFF`) that asserts OpenSSL
  and libssh2 are absent from the build.

## Packaging

`conan create .` builds a consumer-grade package and runs the unit suite; a `test_package/` proves
`find_package` + link + run from a downstream project (no daemon needed). A CI job creates the
package on Linux, Windows, and macOS. The version lives in exactly one place —
`TC_VERSION_FULL` in `CMakeLists.txt` — from which `project()`, `version.cpp`, and the Conan recipe's
`set_version()` all derive it.

A ConanCenter-shaped recipe is staged verbatim in
[`packaging/conan-center/`](packaging/conan-center/) (release-tarball sources pinned by sha256, no
forced dependency options, verified against a fully compiled default Boost on gcc/clang/msvc). It is
kept byte-in-sync with the in-repo `test_package/` by a CI check. Submission is planned right after
the `v0.1.0` tag; the process and the recipe-vs-recipe split are documented in its
[README](packaging/conan-center/README.md).

## History

This is a from-scratch native rewrite. A previous attempt, `testcontainers-cxx/` (kept in this repo
for reference only), wrapped the Rust `testcontainers` library over a cxx FFI bridge. This project
drops the bridge and reimplements everything in idiomatic C++ — plain copyable value types,
in-place-mutating builders, RAII only where a real resource is owned, and a structured exception
hierarchy over a transport layer we own.

## License

Licensed under either of

- MIT license ([LICENSE-MIT](LICENSE-MIT))
- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE))

at your option (`SPDX: MIT OR Apache-2.0`).

Unless you explicitly state otherwise, any contribution intentionally submitted for inclusion in
this work by you, as defined in the Apache-2.0 license, shall be dual licensed as above, without any
additional terms or conditions.
