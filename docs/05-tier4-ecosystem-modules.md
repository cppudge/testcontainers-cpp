# Tier 4 — Ecosystem: prebuilt modules + CI/Cloud

Tiers 1–3 finished the **GenericContainer core**: full container config, build-from-Dockerfile,
copy in/out, follow/streaming logs, interactive exec (stdin/tty/streaming), TTY containers,
wait strategies, lifecycle hooks + startup retry, networks, named volumes, reuse, TLS, credential
helpers, Compose, and rich port/inspect getters.

Tier 4 is the **ecosystem layer**: thin, opinionated wrappers over `GenericImage` for the
databases / brokers / cloud emulators people actually test against, plus the CI/Cloud plumbing.
This is the "long tail" — each module is small, but there are many, so the work is breadth, not depth.

Done in the same cycle as Tiers 1–3: one step per subagent → review the diff → fix → TODO note →
commit → verify on the live daemon.

---

## 1. Scope & guiding principles

1. **A module is a thin wrapper over `GenericImage`** that bakes in: a pinned default image, exposed
   ports, a default wait strategy, domain config setters (user/password/db/…), and domain getters
   (connection string + credentials + host/port). No new transport or protocol code.
2. **Connection strings, not drivers.** We do NOT link libpq / mysqlclient / librdkafka / mongo-c.
   A module returns a DSN/URL/host/port; the user plugs in their own client library. This keeps the
   dependency footprint at zero and matches what a C++ user expects (they already have their driver).
3. **Modules are opt-in.** They live behind their own CMake target(s) so the core library stays lean
   and a user links only the modules they use.
4. **Modules reuse Tier 1–3 primitives.** Readiness = existing log/port/healthcheck/(new) exec waits;
   post-start initialization (Mongo replica set, Kafka advertised listeners) = lifecycle hooks +
   `exec` + `copy_to` from Tier 3. No module should need a new core feature; if one does, that core
   gap is its own step first.
5. **Pin images** (e.g. `postgres:16-alpine`, not `:latest`) for reproducibility; expose an override.
6. **Test by exec-ing the in-container client.** Without a C++ DB driver we prove readiness/behaviour
   by `exec`-ing the image's own CLI (`pg_isready`/`psql`, `redis-cli`, `mongosh`, `rabbitmqctl`,
   kafka topic scripts, `awslocal`) and asserting the connection string is well-formed.

---

## 2. Module design pattern (decide once, in 4.0)

Two shapes to choose between; **recommendation: the "config builder → Started handle" pair**, because
our connection-string getters need the *mapped* port + credentials, which only exist after `start()`.

```cpp
// include/testcontainers/modules/PostgreSQLContainer.hpp
namespace testcontainers::modules {

class StartedPostgreSQL;                 // the running handle (owns a Container)

class PostgreSQLContainer {              // copyable config builder, like GenericImage
public:
    explicit PostgreSQLContainer(std::string image = "postgres:16-alpine");
    PostgreSQLContainer& with_username(std::string u) &;   // + && overload (house style)
    PostgreSQLContainer& with_password(std::string p) &;
    PostgreSQLContainer& with_database(std::string d) &;
    PostgreSQLContainer& with_image(std::string image) &;  // override the pin
    GenericImage to_generic() const;     // escape hatch: drop down to the raw builder
    StartedPostgreSQL start() const;
private:
    std::string image_, username_{"test"}, password_{"test"}, database_{"test"};
};

class StartedPostgreSQL {                // move-only, owns the Container (RAII teardown)
public:
    std::string host() const;            // delegates to Container::host()
    std::uint16_t port() const;          // first_mapped_port() of 5432
    const std::string& username() const;
    const std::string& password() const;
    const std::string& database() const;
    std::string jdbc_url() const;        // "jdbc:postgresql://host:port/db"
    std::string connection_string() const; // "postgresql://user:pass@host:port/db"
    Container& container();              // exec/logs/copy escape hatch
private:
    Container container_;
    std::string username_, password_, database_;
};
}
```

Notes:
- `start()` builds a `GenericImage` from the config (image, `with_exposed_port`, env, a default
  `with_wait(...)`), starts it, and returns a `StartedPostgreSQL` carrying the creds so the getters
  are pure (no re-inspect for creds; port comes from the `Container`).
- Inheritance from `GenericImage` does NOT fit (its `with_*` are non-virtual and return `GenericImage&`),
  so use **composition** + a `to_generic()` escape hatch for power users.
- Header layout: `include/testcontainers/modules/<Name>Container.hpp`, impl `src/modules/<Name>Container.cpp`,
  test `tests/integration/modules/<Name>Test.cpp`. Source files are explicit in CMakeLists (no glob).

---

## 3. Tier 4.0 — Foundation (do these FIRST, before any module)

| Step | What | Why |
|---|---|---|
| 4.0a | **Module pattern + CMake target** — settle the builder/Started shape above; add a `testcontainers_modules` library target (or per-module INTERFACE libs) linking `testcontainers::testcontainers`. One trivial module (Redis, we already have the MVP logic) to validate the pattern end-to-end. | Establishes the convention every later module copies. |
| 4.0b | **Exec-based wait strategy** — `wait_for::Command` / `wait_for::successful_command({"pg_isready","-U","test"})`: poll `exec` until exit code 0 (or a predicate on stdout), under the shared deadline. | DB modules need "the server accepts connections", which a log line alone races; this is `Wait.forSuccessfulCommand` in tc-java. Lives in the core wait subsystem, reused by many modules. |
| 4.0c | **Connection-string / DSN helper** — small pure builder (scheme, user, pass, host, port, db, query params) with URL-encoding, unit-tested. | Every DB/broker module emits one; centralize the escaping. |
| 4.0d | **`host()` correctness for container-to-container / DinD** — honor `TESTCONTAINERS_HOST_OVERRIDE` (and the docker-host gateway) so `host()` returns a daemon-reachable address when tests run *inside* a container, not "localhost". | Required for CI-in-container and Testcontainers Cloud; currently `host()` assumes localhost for socket/pipe. |
| 4.0e | **Module integration-test harness** — a tiny helper + convention: start module → `exec` the in-container client → assert + assert connection-string shape; engine-guarded skip like the existing suites. | Keeps the (many) module tests uniform and daemon-only. |

---

## 4. Module catalog

Each module's "definition of done": pinned default image (+ override), exposed port(s), a sensible
default wait, the domain config setters, the domain getters (connection string + creds + host/port),
one engine-guarded integration test that execs the in-container client. Gotchas and Tier-3 dependencies
are called out.

### 4.A Databases
| Module | Image | Port | Default wait | Config | Getters | Gotchas / deps |
|---|---|---|---|---|---|---|
| **PostgreSQL** | `postgres:16-alpine` | 5432 | exec `pg_isready` (4.0b) or log "ready to accept connections" ×2 | user/pass/db | `jdbc_url`, `connection_string` | log line appears twice (init + real start) — prefer the exec wait |
| **MySQL** | `mysql:8.4` | 3306 | exec `mysqladmin ping` | root pw, user/pass/db | `jdbc_url`, dsn | `MYSQL_ALLOW_EMPTY_PASSWORD`/root-pw rules; slow first boot |
| **MariaDB** | `mariadb:11` | 3306 | exec `healthcheck.sh`/ping | as MySQL | dsn | near-clone of MySQL — share code |
| **MongoDB** | `mongo:7` | 27017 | log "Waiting for connections" | — | `connection_string` (`mongodb://…`) | **replica-set mode** for transactions: post-start hook (Tier 3.3) `exec rs.initiate()` then wait for PRIMARY |
| **MS SQL Server** | `mcr.microsoft.com/mssql/server:2022-latest` | 1433 | exec `sqlcmd` SELECT 1 | `ACCEPT_EULA=Y`, strong SA pw | dsn | EULA env + password-complexity required or it won't boot |
| **CockroachDB / ClickHouse / Neo4j / Cassandra** | respective | — | log/exec | — | dsn / bolt url | long tail; do after the big four |

### 4.B Search
| **Elasticsearch** | `docker.elastic.co/elasticsearch/elasticsearch:8.x` | 9200/9300 | http wait `GET /` 200 | `discovery.type=single-node`, optional security off, password | `http_host_address`, creds | heavy; security-on vs off; `xpack` password. **OpenSearch** is a near-clone. |

### 4.C Messaging
| Module | Image | Port | Notes |
|---|---|---|---|
| **Kafka** | `confluentinc/cp-kafka` or `apache/kafka:3.x` | 9093 (host listener) | **The hard one.** Advertised listeners must point at `host:mappedPort`, unknown until after start → the testcontainers "starter script" trick: copy a small script in (Tier 3 `copy_to`), set a placeholder command, and on a **started hook** (Tier 3.3) `exec` the reconfigure using `first_mapped_port()` then signal the broker to continue. Leans entirely on Tier 3. Getter: `bootstrap_servers()`. |
| **Redpanda** | `redpandadata/redpanda` | 9092 | simpler Kafka-API alternative; similar advertised-listener handling but lighter |
| **RabbitMQ** | `rabbitmq:3-management` | 5672 / 15672 | wait log "Server startup complete"; getters `amqp_url`, mgmt url; optional vhost/user via env |
| **Pulsar / NATS** | respective | — | long tail |

### 4.D Cloud / misc
| Module | Image | Port | Notes |
|---|---|---|---|
| **LocalStack** | `localstack/localstack:3` | 4566 (edge) | `SERVICES=s3,sqs,…` env; wait "Ready."; getters: endpoint url + region + dummy creds; test via `awslocal s3 mb` exec |
| **MinIO** | `minio/minio` | 9000/9001 | S3-compatible; access/secret key env; `s3_url()` |
| **Vault / Consul** | hashicorp images | 8200 / 8500 | dev-mode root token; getters: address + token |
| **Toxiproxy** | `ghcr.io/shopify/toxiproxy` | 8474 + proxied | network fault injection — also useful infra; bigger surface (proxy/​toxic API) |
| **Nginx / httpd** | `nginx:alpine` | 80 | trivial; good smoke/demo module |
| **MockServer / WireMock** | respective | 1080 / 8080 | HTTP mocking; getters: base url; config = mappings (could copy_to a JSON) |

### 4.E Browser (heaviest — defer/optional)
| **Selenium / WebDriver** | `selenium/standalone-chrome` (+ firefox) | 4444 (+ 7900 VNC) | getters: `webdriver_url` (RemoteWebDriver address); optional **VNC video recording** sidecar (a second container recording to a file, started/stopped around the test). Large; only if there's demand. |

---

## 5. Tier 4.x — CI / Cloud infrastructure

| Step | What |
|---|---|
| **DinD / socket-mount** | Document + verify running the test process inside a container with `/var/run/docker.sock` bind-mounted or a DinD daemon: relies on host resolution (done) + `host()` override (4.0d). Add a note/recipe; possibly a CI smoke test. |
| **Testcontainers Cloud (TCC)** | Works when `DOCKER_HOST` points at the TCC agent and `TC_CLOUD_TOKEN` is set; mostly "it works if host resolution + `TESTCONTAINERS_HOST_OVERRIDE` are correct". Verify our resolver handles a remote TCP daemon + the host override; document `TC_CLOUD_TOKEN`/agent setup. Minimal new code. |
| **Ryuk on remote/Cloud** | Confirm Ryuk reaping still works against a remote daemon (it should — Ryuk runs daemon-side); note any caveat. |

---

## 6. Recommended sequencing

1. **4.0a–4.0e foundation** (pattern, exec-wait, DSN helper, host-override, test harness) — unblocks everything.
2. **High-value modules, in order:** PostgreSQL → MySQL (+ MariaDB share) → MongoDB (replica set) →
   Redis (formalize the MVP) → LocalStack → Kafka (the showcase for Tier-3 hooks/exec/copy) →
   RabbitMQ → Elasticsearch/OpenSearch.
3. **Long tail** as demand dictates: MS SQL, MinIO, Vault/Consul, Toxiproxy, Nginx, MockServer,
   CockroachDB/ClickHouse/Neo4j/Cassandra.
4. **CI/Cloud** (DinD doc + TCC verification) — can slot in any time after 4.0d.
5. **Selenium** last (heaviest, narrowest audience).

## 7. Cross-cutting notes / risks

- **No DB drivers** is a deliberate scope line — revisit only if we ever want "real" connection helpers
  (would pull heavy optional deps; keep them optional + per-module if so).
- **Image churn / pull time** — module integration tests pull real images; gate them (engine guard +
  maybe a separate ctest label `modules`) so the fast unit/integration loop isn't slowed.
- **Kafka & Mongo are the integration tests for Tier 3** — they exercise lifecycle hooks + exec +
  copy_to in anger; treat their green tests as validation that Tier 3 holds up under real use.
- **Reaping** — module containers go through `GenericImage::start()`, so they already get the Ryuk
  session labels; sidecars (Selenium VNC recorder) must be labeled too.
