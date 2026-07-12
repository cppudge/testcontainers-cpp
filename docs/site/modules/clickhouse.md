# ClickHouse

`modules::ClickHouseImage` → `modules::ClickHouseContainer` — pinned **`clickhouse:26.3`**
(the Docker Official Image, current LTS line), HTTP port 8123 + native-protocol port 9000,
credentials **test/test/test**. Readiness is an *ordered triple*: (1) the entrypoint has
handed the container over to the real server — the image's first boot runs a temporary
server for provisioning and init scripts that network probes cannot reliably tell from the
real one (on Docker Desktop even the published port reaches it), but the entrypoint
finishing by `exec`-ing the server over itself can be observed (`/proc/1/comm` flips);
(2) an HTTP `/ping` probe through the published host port — the end-to-end proof;
(3) an in-container `SELECT 1` over the native protocol — the native listener opens a beat
after the HTTP one, and this also proves the provisioned credentials. Every init script
has finished by (1).

## Quick start

```cpp
#include "testcontainers/modules/ClickHouse.hpp"

using namespace testcontainers;

const modules::ClickHouseContainer ch = modules::ClickHouseImage().start();

ch.host();                 // "localhost" for a local daemon
ch.native_port();          // published host port for 9000 (clickhouse-cpp, CLI)
ch.http_port();            // published host port for 8123 (curl, JDBC, ODBC)
ch.connection_string();    // "clickhouse://test:test@localhost:<nport>/test"
ch.http_url();             // "http://localhost:<hport>"
```

For **clickhouse-cpp** take the discrete pieces — its `ClientOptions` has no DSN parser:

```cpp
clickhouse::Client client(clickhouse::ClientOptions()
                              .SetHost(ch.host())
                              .SetPort(ch.native_port())
                              .SetUser(ch.username())
                              .SetPassword(ch.password())
                              .SetDefaultDatabase(ch.database()));
```

## Configuration

```cpp
const modules::ClickHouseContainer ch =
    modules::ClickHouseImage()
        .with_username("app")
        .with_password("s3cr3t")
        .with_database("appdb")
        .with_init_script("schema.sql",
                          "CREATE TABLE events (id UInt64) ENGINE = MergeTree ORDER BY id")
        .with_config_file("clickhouse-tuning.yml")   // -> /etc/clickhouse-server/config.d
        .start();
```

- **Credentials** are provisioned at first boot; the configured user may connect from any
  address and **replaces** the image's built-in `default` user. An empty password throws at
  `start()`: the image restricts a passwordless server to the container's loopback, which
  the module's host-side getters could never describe.
- **`with_init_script`** (host file or in-memory) targets `/docker-entrypoint-initdb.d`;
  scripts run once, at first boot, in **registration order** (the module adds a zero-padded
  index prefix). Extensions the entrypoint executes: `.sql`, `.sql.gz`, `.sh` — anything
  else throws instead of being silently skipped. The default probes wait for all of them.
  Two entrypoint contracts to know: scripts run with **no default database** — qualify
  names (`CREATE TABLE test.t ...`) or open with `USE <db>;` — and a **failing script
  aborts the boot** (`start()` fails loudly; the container log carries the error).
- **`with_config_file`** ships a server-config drop-in (`.xml` / `.yaml` / `.yml`) into
  `config.d`. Don't remap `http_port` / `tcp_port` / `listen_host` — that breaks the port
  getters and the readiness probe.
- **`exec_sql(sql)`** runs one statement through the in-container `clickhouse-client`
  (native protocol, provisioned user). Output is TabSeparated — rows by line, columns by tab.

## Connecting

`connection_string()` renders the native-protocol DSN (`clickhouse://user:pass@host:port/db`,
percent-encoded) that DSN-taking clients accept; HTTP consumers build on `http_url()`
(credentials go per request — basic auth or the `X-ClickHouse-User` / `X-ClickHouse-Key`
headers). Peer containers on a shared docker network dial `<alias>:8123` / `<alias>:9000`
with the same credentials.

## Behavior notes

- The module owns the three `CLICKHOUSE_*` credential env keys (appended last — they win
  over raw `with_env` duplicates) and nothing else; the image's other knobs
  (`CLICKHOUSE_DEFAULT_ACCESS_MANAGEMENT`, ...) stay yours via `with_env`. Leave
  `CLICKHOUSE_SKIP_USER_SETUP` unset — it disables the provisioning the getters describe.
- `with_wait` **replaces** the default readiness triple. Mind the first-boot provisioning
  server: network and loopback probes read ready against it — gate on
  `wait_for::successful_shell_command("grep -q clickhouse /proc/1/comm")` first, as the
  default triple does.
- A reused (adopted) container keeps its data; init scripts are not re-run. Editing a
  script changes the reuse hash, so the next `start()` builds a fresh container.
- Single-node by design; clusters, replicas, and ClickHouse Keeper are out of scope.
