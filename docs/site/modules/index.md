# Modules

Modules are prebuilt technology wrappers over `GenericImage` (new in 0.2.0): a pinned image,
the *right* readiness probe for that technology, and typed connection getters — so a test
starts with a connection string instead of image/wait/port boilerplate. No client drivers
are pulled in: the handles hand out host/port and DSN strings; your project brings its own
driver.

```cpp
#include "testcontainers/modules.hpp"   // umbrella; per-module headers exist too

using namespace testcontainers;

const modules::PostgreSQLContainer pg = modules::PostgreSQLImage().start();
// pg.connection_string() -> "postgresql://test:test@localhost:<port>/test"
```

Link `testcontainers::modules` (its own library target and Conan component):

```cmake
target_link_libraries(my_tests PRIVATE testcontainers::modules)
```

## The pattern

Every module is the same two-class split, deliberately mirroring the core
`GenericImage` → `Container` pair:

- **`XxxImage`** — a copyable config **builder** ready to `start()` with zero configuration.
  Rendering happens at `start()`: the module validates its config up front (throws before
  any daemon contact) and layers its managed settings deterministically.
- **`XxxContainer`** — the move-only **started handle**, owning the core `Container`. Host,
  port, and credentials are resolved **once at `start()`**, so the connection getters are
  pure (`noexcept`, no daemon round-trips). If you restart the container by hand, ephemeral
  ports change — drop to `container()` to re-resolve.

Every module surfaces the same curated pass-throughs to the embedded builder: `with_env`,
`with_label`, `with_network` (name or `Network`), `with_network_alias`, `with_reuse`,
`with_startup_timeout`, `with_startup_attempts`. Two escape hatches cover everything else:

- **`with_customizer(fn)`** — a callback over the underlying `GenericImage`, run at render
  time **after** the module's own rendering, so what it sets wins.
- **`to_generic()`** — renders the full config into a plain `GenericImage` for when a raw
  core `Container` is wanted.

!!! note "Startup timeout is per-phase"
    `with_startup_timeout` budgets each startup **phase**. Most modules have one phase;
    Kafka and MongoDB run a second, hook-driven phase (listener rewrite / replica-set
    initiation) with a fresh allowance of the same size — worst case ≈ 2× the value.

Environment precedence is part of each module's contract: the database modules append their
managed credential keys **after** your `with_env` entries (the module wins — the getters can
never disagree with the server), while Kafka appends your entries after its own (broker
tuning — you win). Redis (its `REDISCLI_AUTH` env key) and NATS (its managed server flags)
guard theirs with a render-time error instead.

## The modules

| Module | Classes | Image pin | Ports | Highlights |
|---|---|---|---|---|
| [Redis](redis.md) | `RedisImage` → `RedisContainer` | `redis:7.2` | 6379 | `connection_string(db)`, `with_password` wires `redis-cli` auth |
| [PostgreSQL](postgresql.md) | `PostgreSQLImage` → `PostgreSQLContainer` | `postgres:16-alpine` | 5432 | init scripts, `conninfo()`, `exec_sql()` |
| [MySQL](mysql.md) | `MySQLImage` → `MySQLContainer` | `mysql:8.4` | 3306 | init scripts, `.cnf` drop-ins, root-password matrix |
| [MariaDB](mariadb.md) | `MariaDBImage` → `MariaDBContainer` | `mariadb:11` | 3306 | same surface as MySQL, MariaDB-native probe |
| [Kafka](kafka.md) | `KafkaImage` → `KafkaContainer` | `apache/kafka:3.9.1` | 9092 (host) / 9093 (network) | single-node KRaft, `with_topic`, dual bootstrap getters |
| [RabbitMQ](rabbitmq.md) | `RabbitMQImage` → `RabbitMQContainer` | `rabbitmq:3.13-management` | 5672 + 15672 | `amqp_url()`, definitions import, `with_plugin` |
| [MongoDB](mongodb.md) | `MongoDBImage` → `MongoDBContainer` | `mongo:7` | 27017 | single-node replica set — transactions work; `mongosh()` |
| [NATS](nats.md) | `NATSImage` → `NATSContainer` | `nats:2.12` | 4222 + 8222 | `url()`, `with_jetstream`, HTTP monitoring API |
| [Mosquitto](mosquitto.md) | `MosquittoImage` → `MosquittoContainer` | `eclipse-mosquitto:2.0` | 1883 | managed conf (anonymous remote clients), `with_config_option`, `broker_url()` |
| [ClickHouse](clickhouse.md) | `ClickHouseImage` → `ClickHouseContainer` | `clickhouse:26.3` | 8123 + 9000 | init scripts, config drop-ins, `exec_sql()`, dual HTTP/native endpoints |
| [MinIO](minio.md) | `MinIOImage` → `MinIOContainer` | `minio/minio:RELEASE.2025-09-07T16-13-09Z` | 9000 + 9001 | S3 endpoint + credential getters, `with_bucket` via the in-image `mc` |
| [RustFS](rustfs.md) | `RustFSImage` → `RustFSContainer` | `rustfs/rustfs:1.0.0-beta.8` | 9000 + 9001 | MinIO-compatible getter surface, `/health` probe, beta pin |
| [ScyllaDB](scylladb.md) | `ScyllaDBImage` → `ScyllaDBContainer` | `scylladb/scylla:2026.1` | 9042 | CI-shape flags, `contact_point()`/`datacenter()`, `.cql` init scripts, `exec_cql()` |

Headers live under `testcontainers/modules/<Name>.hpp`, one per module (both classes);
`testcontainers/modules.hpp` includes them all.
