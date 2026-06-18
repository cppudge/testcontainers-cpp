# testcontainers-cpp

Native C++ port of [Testcontainers](https://testcontainers.com/) — spin up real Docker
containers from your integration tests and tear them down automatically. **No Rust, no `docker`
CLI**: a pure C++ client that talks to the Docker Engine HTTP API directly.

> **Status:** early alpha / design phase. The build skeleton and dependency stack are in place;
> the Docker client and public API are being implemented. See the roadmap below.

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
- **Builders mutate in place and chain** via ref-qualified `with_*` (`T& ... &` / `T&& ... &&`).
  No "consume-self-return-Self" — named configs are reusable, no use-after-move footguns.
- **No trait-mirror interfaces.** Runtime polymorphism only where there's real extension:
  a user-facing `Image` base (custom images) and optionally `IWaitStrategy`.
- **`WaitFor` is a `std::variant`** of small structs, matched with `std::visit`.
- **RAII only where a real resource is owned** — the running `Container` (auto-removed on
  destruction); everything else is a value.
- **Structured errors** (exception hierarchy carrying HTTP status / container id), since we own
  the HTTP layer.

## Project layout

```
include/testcontainers/      public API headers (core/, core/wait/, system/ip/)
src/                         implementation
  docker/                    Docker Engine HTTP API client (transport + endpoints)
examples/                    runnable examples (smoke test)
tests/unit/                  unit tests (GoogleTest)
cmake/cmake-conan/           vendored cmake-conan provider
docs/                        design docs (01 architecture, 02 deps, 03 interface audit)
_research/testcontainers-rs/ cloned Rust reference sources
testcontainers-cxx/          previous FFI-bridge fork (reference only)
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
- [ ] Wait strategies: log message (polling) + fixed duration done; http / healthcheck / exit pending,
      60s startup timeout
- [x] **MVP**: `GenericImage("redis","7.2")` up → connect → auto-remove
- [ ] Cleanup: RAII + Ryuk reaper; networks, mounts, copy, exec, registry auth

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

> Note: `libarchive` is built with `with_iconv=False` — its `libiconv` dependency has no prebuilt
> Windows/msvc-194 binary and fails to build from source (rc.exe flag mismatch); we don't need
> iconv for tar.

## License

Dual-licensed under MIT or Apache-2.0, at your option.
