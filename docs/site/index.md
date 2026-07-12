# testcontainers-cpp

Native C++20 port of [Testcontainers](https://testcontainers.com/) — spin up real Docker
containers from your integration tests and tear them down automatically. **No `docker` CLI
required**: a single C++ library speaking the Docker Engine HTTP API directly, with full
streaming control over pulls, logs, and exec.

[Get started](getting-started.md){ .md-button .md-button--primary }
[Browse the modules](modules/index.md){ .md-button }
[API reference](https://cppudge.github.io/testcontainers-cpp/api/){ .md-button }

Run your tests against the real thing — Postgres, Redis, a broker, your own image — instead of
a mock. Each test owns an isolated, disposable environment, created on `start()` and
force-removed when its handle leaves scope (RAII), with a crash-safety reaper as backstop.

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

Or start from a prebuilt [module](modules/index.md) — pinned image, the right readiness
probe, and connection strings out of the box:

```cpp
#include "testcontainers/modules.hpp"

const modules::PostgreSQLContainer pg =
    modules::PostgreSQLImage()
        .with_init_script("schema.sql", "CREATE TABLE t(id int);")
        .start(); // returns once every init script ran and TCP serves

// pg.connection_string() -> "postgresql://test:test@localhost:<port>/test"
```

## Feature highlights

- **Images** — the `GenericImage` builder (env, cmd, mounts, healthcheck, registry auth, …)
  and `GenericBuildableImage` (build from a Dockerfile + context, live build-log streaming).
- **Seven wait strategies** — log message, fixed duration, exit code, Docker healthcheck,
  HTTP probe, listening port, successful in-container command; chained under one startup
  timeout.
- **Containers** — typed `inspect`, snapshot + streaming `logs`, `exec` (env / tty / stdin /
  streaming), copy to/from, host-port getters.
- **Networking & volumes** — user-defined `Network` and named `Volume` handles, RAII and
  session-labeled for cleanup.
- **Lifecycle** — hooks, startup retry, container **reuse**, and the
  [Ryuk](https://github.com/testcontainers/moby-ryuk) reaper for crash-safe cleanup.
- **Compose & host access** — `DockerComposeContainer` (CLI / containerised / auto client
  modes) and `with_exposed_host_port` (reach services on the test host from containers).
- **Windows containers** — engine-mode detection, `with_platform` / `with_isolation`, and a
  full Windows-engine test mirror in CI.
- **Modules** — Redis, PostgreSQL, MySQL, MariaDB, Kafka, RabbitMQ, MongoDB, NATS,
  Mosquitto, ClickHouse, MinIO, RustFS, ScyllaDB: pinned images, load-bearing readiness
  probes, typed connection getters.

## Platform support

CI builds and runs the full suite against a real daemon in **both** engine modes:

| Platform | Docker endpoint | CI coverage |
|---|---|---|
| Linux | unix socket | full unit + integration suite (gcc) |
| Windows | named pipe | full suite against **real Windows containers** (MSVC) |
| macOS | unix socket (Docker Desktop) | `conan create` + unit suite (apple-clang) |
| Remote | `tcp://` / `https://` | mutual TLS end-to-end in CI (a real `--tlsverify` docker:dind daemon) |

## How it works

Testcontainers is, at its core, a **client over the Docker Engine HTTP API**:

1. Resolve where the Docker daemon listens (env vars → socket/pipe → defaults).
2. `POST /containers/create` → `POST /containers/{id}/start`.
3. **Wait for readiness** (log message, HTTP probe, healthcheck, …).
4. `GET /containers/{id}/json` → discover the **host port** Docker published.
5. Hand the user a handle to talk to the container.
6. On scope exit, `DELETE /containers/{id}` (RAII), with the Ryuk reaper as crash-safety net.

Every transport is HTTP/1.1 over a single Boost.Asio stream abstraction — unix socket,
Windows named pipe, TCP, or TLS — chosen by the resolved endpoint's scheme.

## License

Dual-licensed under [MIT](https://github.com/cppudge/testcontainers-cpp/blob/main/LICENSE-MIT)
or [Apache-2.0](https://github.com/cppudge/testcontainers-cpp/blob/main/LICENSE-APACHE), at
your option.
