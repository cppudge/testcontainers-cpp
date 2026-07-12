# Redis

`modules::RedisImage` → `modules::RedisContainer` — pinned **`redis:7.2`**, port 6379.
Readiness is an in-container `redis-cli ping` (a log wait races the listener; a raw TCP
probe false-positives through Docker Desktop's host proxy).

## Quick start

```cpp
#include "testcontainers/modules/Redis.hpp"

using namespace testcontainers;

const modules::RedisContainer redis = modules::RedisImage().start();

redis.host();                  // "localhost" for a local daemon
redis.port();                  // the published host port
redis.connection_string();     // "redis://localhost:<port>"
redis.connection_string(3);    // "redis://localhost:<port>/3"
```

## Configuration

```cpp
const modules::RedisContainer redis =
    modules::RedisImage()
        .with_password("s3cr3t")                       // --requirepass + REDISCLI_AUTH
        .with_command_args({"--maxmemory", "64mb"})    // extra redis-server arguments
        .start();

// "redis://:s3cr3t@localhost:<port>"
const std::string url = redis.connection_string();
```

- **`with_password(pw)`** renders `redis-server --requirepass <pw>` **and** sets
  container-level `REDISCLI_AUTH`, so the readiness probe — and any `redis-cli` you `exec`
  yourself — authenticates automatically.
- **`with_command_arg` / `with_command_args`** append `redis-server` arguments after
  `--requirepass`; argv[0] stays `redis-server`, so the official entrypoint's
  protected-mode handling keeps applying.
- Redis has no server-side database setter — selection is client-side, via
  `connection_string(db)`.

## Behavior notes

- The module owns the container command (iff a password or args are set) and the
  `REDISCLI_AUTH` env key — nothing else. Passing `REDISCLI_AUTH` through `with_env`
  alongside `with_password` throws at render time: exec'd `redis-cli` reads the *first*
  duplicate of an env key, so no ordering could make the module's entry win.
- A Redis config **file** must be the first server argument, so a config file combined with
  `with_password` is unsupported — drop to a customizer's `with_cmd` for full control.
- redis-stack, Sentinel, and cluster topologies are out of scope (different image families /
  multi-container setups).
