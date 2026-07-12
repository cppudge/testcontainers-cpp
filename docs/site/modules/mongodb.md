# MongoDB

`modules::MongoDBImage` → `modules::MongoDBContainer` — pinned **`mongo:7`** (floor
`mongo:5.0` — the module execs `mongosh`, which older images don't ship), port 27017,
default database `test`.

The module **always** runs a single-node replica set (`--replSet rs0 --bind_ip_all`):
transactions and change streams are why a MongoDB test module exists, and a standalone
server rejects both. The whole cost is a ~1–2 s election on top of the boot; there is
deliberately no standalone mode. The replica set is initiated and awaited by a started
hook — from the outside it is just `start()`.

## Quick start

```cpp
#include "testcontainers/modules/MongoDB.hpp"

using namespace testcontainers;

const modules::MongoDBContainer mongo = modules::MongoDBImage().start();

// "mongodb://localhost:<port>/test?directConnection=true"
const std::string uri = mongo.connection_string();

// Seed and assert without a C++ driver:
mongo.mongosh("db.users.insertOne({name: 'ada'})");
const ExecResult r = mongo.mongosh("db.users.countDocuments()");
EXPECT_EQ(r.stdout_data, "1\n");
```

## Connection string

`connection_string([database])` emits `mongodb://host:port/<db>?directConnection=true` —
and **never** `replicaSet=`. Direct mode pins single-server behavior in every spec-compliant
driver, instead of relying on per-driver legacy defaults (the classic "works in
testcontainers-java, `ServerSelectionTimeoutError` in PyMongo"). A direct connection to a
PRIMARY fully supports sessions, transactions, and change streams. The database segment is
always present — strict URI parsers reject options without the slash.

## Configuration

- **`with_database(name)`** — the default target of `connection_string()` and `mongosh()`.
- **`with_replica_set_name(name)`** — replaces the default `rs0`.
- **`with_env`** — passes through, with one guard: `MONGO_INITDB_ROOT_USERNAME` /
  `MONGO_INITDB_ROOT_PASSWORD` are **rejected at render**. MongoDB requires a cluster
  keyfile the moment auth meets a replica set, so those keys are a boot-breaker under
  `--replSet` — the same reason the module has no auth surface at all.

## Behavior notes

- `/docker-entrypoint-initdb.d` scripts are unsupported for the same reason: they trigger a
  temporary double-start whose log line would release the wait early. Seed through
  `mongosh()` after start instead.
- [Reuse](../core-concepts.md#cleanup-raii-ryuk)-adopted containers skip the initiation
  hook — correct, because the replica-set config persists in the data directory.
- `with_startup_timeout` budgets each phase (container boot, then replica-set
  initiation + PRIMARY election) with a fresh allowance of the same size.
- Out of scope: auth/keyfile choreography, multi-node sets, sharding.
