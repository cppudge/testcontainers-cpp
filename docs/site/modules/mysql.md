# MySQL

`modules::MySQLImage` → `modules::MySQLContainer` — pinned **`mysql:8.4`**, port 3306,
default credentials `test`/`test`, database `test`, default startup budget **120 s** (a
first boot initializes the datadir). Readiness is an in-container
`mysqladmin ping -h127.0.0.1` — forced over TCP because the image's first boot runs a
*temporary* socket-only bootstrap server (credential provisioning + init scripts) that
prints the same "ready for connections" log line as the real server.

MySQL and [MariaDB](mariadb.md) share one implementation core; the surface below applies to
both, with the flavor differences called out on each page.

## Quick start

```cpp
#include "testcontainers/modules/MySQL.hpp"

using namespace testcontainers;

const modules::MySQLContainer db =
    modules::MySQLImage()
        .with_init_script("schema.sql", "CREATE TABLE t(id INT);")
        .start();

db.connection_string();   // "mysql://test:test@localhost:<port>/test"
db.root_password();       // == db.password() — see the root matrix below
```

## The root-password matrix

Every start carries a deliberate root decision:

| Config | Result |
|---|---|
| non-root user (default) | the user **shares its password with root** — a known superuser on every path |
| `with_username("root")` | root-only provisioning (no `MYSQL_USER` key — the image refuses `user=root`) |
| root + empty password | renders the image's allow-empty key |
| non-root + empty password | **fails fast at render** (the image's own failure mode is an entrypoint error plus the full wait budget) |

`root_password()` on the started handle documents that invariant at call sites.

## Configuration

- **`with_username` / `with_password` / `with_database`** — the credential trio, appended
  after your `with_env` entries (module wins — the getters stay truthful).
- **`with_init_script(...)`** — same staging rules as
  [PostgreSQL](postgresql.md#configuration): registration-order prefix, extension whitelist,
  `.sh` shipped executable.
- **`with_config_file(host_cnf)`** — a `.cnf` drop-in for `/etc/mysql/conf.d`. The name must
  end in `.cnf` — the image's include glob silently skips anything else, so the module
  enforces it.
- **`with_command_arg` / `with_command_args`** — become the container command verbatim (the
  entrypoint forwards `-`-prefixed arguments to the server binary).

## Connecting

`host()` / `port()` / `username()` / `password()` / `root_password()` / `database()`, and
`connection_string()` → `mysql://user:pass@host:port/db`.

## Behavior notes

- MySQL 8.4 disables the `mysql_native_password` plugin by default. Pre-8.0-era client
  stacks need `with_command_arg("--mysql-native-password=ON")` plus an `ALTER USER` init
  script.
- The readiness probe measures **liveness**, not credentials — `mysqladmin ping` exits 0
  even on access-denied, which makes it immune to credential edge cases.
