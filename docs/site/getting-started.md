# Getting started

## Requirements

- **C++20** — the public headers require it (they will not parse under C++17).
- **Docker** reachable from the test host (local daemon, Docker Desktop, or a remote engine).
- **CMake ≥ 3.20** and **Conan ≥ 2** to consume the package (building this repository
  itself from source uses its presets — CMake ≥ 3.21 there).

Supported compilers (enforced by the Conan recipe):

| Compiler | Minimum |
|---|---|
| GCC | 12 |
| Clang | 15 |
| MSVC | 19.3 (Visual Studio 2022) |
| Apple Clang | 15 |

## Install

testcontainers-cpp is a **Conan 2** package. The ConanCenter submission is in review
([conan-io/conan-center-index#30600](https://github.com/conan-io/conan-center-index/pull/30600));
until it lands, build the package from the repository into your local Conan cache:

```sh
git clone https://github.com/cppudge/testcontainers-cpp
conan create testcontainers-cpp --build=missing -s compiler.cppstd=20
```

Then require it from your project's `conanfile.txt`:

```ini
[requires]
testcontainers-cpp/0.2.0

[generators]
CMakeDeps
CMakeToolchain
```

and wire it into CMake with `find_package` (the CMake package and target are named
`testcontainers`):

```cmake
find_package(testcontainers REQUIRED)
target_link_libraries(my_tests PRIVATE testcontainers::testcontainers)
# For the prebuilt technology wrappers (modules::RedisImage, ...):
target_link_libraries(my_tests PRIVATE testcontainers::modules)
```

!!! note "The portable link line"
    Under Conan the root `testcontainers::testcontainers` target already carries the modules
    library; the explicit `testcontainers::modules` line is what keeps the same CMakeLists
    working against a plain `cmake --install` tree, whose root target is core-only.

A plain `cmake --install` of the repository works too, exporting the same
`find_package(testcontainers)` contract for non-Conan consumers.

### Trimming the dependency graph

Two build options (both default **ON**) let consumers who must avoid a transitive OpenSSL
trim the graph: Conan `tls` / CMake `TC_TLS` gates the `https://` transport, and Conan
`host_port_forwarding` / CMake `TC_HOST_PORT_FORWARDING` gates `with_exposed_host_port`'s
sshd sidecar (libssh2). Disabling **both** removes OpenSSL from the dependency graph
entirely; a disabled path throws a `DockerError` naming the option.

## Your first test

```cpp
#include <gtest/gtest.h>
#include "testcontainers/testcontainers.hpp"

using namespace testcontainers;

TEST(Cache, RedisIsReachable) {
    Container redis = GenericImage("redis", "7.2")
                          .with_exposed_port(tcp(6379))
                          .with_wait(wait_for::stdout_message("Ready to accept connections"))
                          .start();

    const std::string   host = redis.host();
    const std::uint16_t port = redis.get_host_port(tcp(6379));

    // ... point your Redis client at host:port and run assertions ...
}
```

What happens here:

1. `GenericImage("redis", "7.2")` describes the container — a copyable, reusable builder;
   each `with_*` mutates in place and chains.
2. `start()` pulls the image if missing, creates and starts the container, and **blocks
   until the wait strategy passes** (or throws `StartupTimeoutError`).
3. `get_host_port` returns the ephemeral host port Docker published for container port 6379.
4. When `redis` leaves scope the container is force-removed. If the test process crashes
   instead, the [Ryuk reaper](core-concepts.md#cleanup-raii-ryuk) sweeps it a few seconds
   later.

Everything lives in `namespace testcontainers`; the umbrella header
`testcontainers/testcontainers.hpp` brings in the whole core API. Errors are thrown, not
returned — a `DockerError` exception hierarchy carrying the HTTP status and resource id.

## Or start from a module

The [modules](modules/index.md) skip the image/wait/port boilerplate for a growing set of
common technologies and add typed connection getters:

```cpp
#include <gtest/gtest.h>
#include "testcontainers/modules.hpp"

using namespace testcontainers;

TEST(Db, PostgresAnswersQueries) {
    const modules::PostgreSQLContainer pg =
        modules::PostgreSQLImage()
            .with_init_script("schema.sql", "CREATE TABLE t(id int);")
            .start();

    const ExecResult r = pg.exec_sql("SELECT count(*) FROM t");
    EXPECT_EQ(r.stdout_data, "0\n");
}
```

## Pointing the library at your daemon

With Docker Desktop or a local daemon, no configuration is needed — the library resolves
the endpoint the same way the `docker` CLI does (`DOCKER_HOST`, docker contexts, platform
defaults, in that order). Remote daemons, TLS, registry credentials, and the
`~/.testcontainers.properties` file are covered in [Configuration](configuration.md).

## Where to next

- [Core concepts](core-concepts.md) — the container lifecycle, wait strategies, cleanup,
  exec/logs, networks, compose.
- [Modules](modules/index.md) — Redis, PostgreSQL, MySQL, MariaDB, Kafka, RabbitMQ, MongoDB,
  NATS, Mosquitto.
- [API reference](https://cppudge.github.io/testcontainers-cpp/api/) — every public class and
  method, generated from the headers.
