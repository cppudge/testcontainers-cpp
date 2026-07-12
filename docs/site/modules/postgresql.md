# PostgreSQL

`modules::PostgreSQLImage` → `modules::PostgreSQLContainer` — pinned **`postgres:16-alpine`**,
port 5432, default credentials `test`/`test`, database `test`. Readiness is an in-container
`pg_isready` forced over TCP — and that choice is load-bearing: the image's first boot runs a
*temporary* unix-socket-only server (initdb + init scripts), shuts it down, then starts the
real TCP server. A socket probe or the "ready to accept connections" log line (printed by
**both** servers) reads ready inside that window; the TCP probe cannot — and it additionally
proves every init script finished.

## Quick start

```cpp
#include "testcontainers/modules/PostgreSQL.hpp"

using namespace testcontainers;

const modules::PostgreSQLContainer pg =
    modules::PostgreSQLImage()
        .with_init_script("schema.sql", "CREATE TABLE t(id int);")
        .start();

pg.connection_string();   // "postgresql://test:test@localhost:<port>/test"
pg.conninfo();            // "host=localhost port=<port> dbname=test user=test password=test"

const ExecResult r = pg.exec_sql("SELECT count(*) FROM t");   // in-container psql
EXPECT_EQ(r.stdout_data, "0\n");
```

## Configuration

- **`with_username` / `with_password` / `with_database`** — the credential trio (rendered as
  `POSTGRES_USER`/`POSTGRES_PASSWORD`/`POSTGRES_DB`, appended after your `with_env` entries
  so the getters can never disagree with the server). An empty password fails fast at render
  time unless you set `POSTGRES_HOST_AUTH_METHOD=trust` yourself.
- **`with_init_script(host_path)` / `with_init_script(name, content)`** — staged into
  `/docker-entrypoint-initdb.d` with a zero-padded registration-index prefix, so scripts run
  in **registration order** (not C-collation name order). Recognized extensions: `.sql`,
  `.sql.gz`, `.sql.xz`, `.sql.zst`, `.sh` — anything else throws instead of being silently
  skipped. `.sh` ships executable (a non-executable `.sh` would be *sourced* into the
  entrypoint's shell, where a stray `exit` kills the boot).
- **`with_config_option(key, value)`** — renders `postgres -c key=value`.
- **`with_wait(strategy)`** — *replaces* the default probe (a customizer-added wait runs in
  addition to it).

## Connecting

| Getter | Returns |
|---|---|
| `host()` / `port()` | resolved once at `start()` |
| `username()` / `password()` / `database()` | the configured credentials |
| `connection_string()` | `postgresql://user:pass@host:port/db` |
| `connection_string_with_scheme("postgres")` | same URI under another scheme spelling |
| `conninfo()` | libpq keyword/value form, with libpq quoting rules |
| `exec_sql(sql)` | runs `psql -X -tA -c <sql>` inside the container (local-socket trust — no password needed) |

## Behavior notes

- The alpine image builds with the **C locale**; point collation-sensitive tests at the
  Debian-based image (`with_image("postgres:16")`) or pass `POSTGRES_INITDB_ARGS`.
- Init scripts are **not** re-run when a [reused](../core-concepts.md#cleanup-raii-ryuk)
  container is adopted — data persistence is the point; editing a script changes the reuse
  hash and creates a fresh container.
