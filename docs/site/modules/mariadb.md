# MariaDB

`modules::MariaDBImage` → `modules::MariaDBContainer` — pinned **`mariadb:11`**, port 3306,
default credentials `test`/`test`, database `test`, default startup budget **120 s**.

The surface is identical to [MySQL](mysql.md) — the two modules share one implementation
core (credential matrix, init-script staging, config drop-ins, rendering). What differs is
the flavor contract:

- **Readiness** uses the image's own `healthcheck.sh --connect --innodb_initialized` —
  credential-free, and it sidesteps the image's renamed client binaries (`mariadb` /
  `mariadb-admin`, not the deprecated `mysql-*` names).
- **Env keys** render under the MariaDB names (`MARIADB_USER`, …).

## Quick start

```cpp
#include "testcontainers/modules/MariaDB.hpp"

using namespace testcontainers;

const modules::MariaDBContainer db =
    modules::MariaDBImage()
        .with_database("app")
        .with_init_script("schema.sql", "CREATE TABLE t(id INT);")
        .start();

db.connection_string();   // "mysql://test:test@localhost:<port>/app"
```

!!! note "Why `mysql://` and not `mariadb://`?"
    MariaDB speaks the MySQL wire protocol, and URL-parsing client libraries widely reject a
    `mariadb://` scheme — so `connection_string()` deliberately emits **`mysql://`** for both
    modules.

## Configuration & connecting

See [MySQL](mysql.md) — `with_username` / `with_password` / `with_database`,
`with_init_script`, `with_config_file` (`.cnf` drop-ins), `with_command_arg[s]`, the
root-password matrix (`root_password()` included), and the same started-handle getters all
apply verbatim.
