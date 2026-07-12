# NATS

`modules::NATSImage` → `modules::NATSContainer` — pinned **`nats:2.12`**, client port 4222
plus the HTTP monitoring API on 8222 (always enabled). Readiness is the "Server is ready"
log line followed by an HTTP `/healthz` probe through the published monitoring port — the
end-to-end proof a raw TCP wait can't give.

## Quick start

```cpp
#include "testcontainers/modules/NATS.hpp"

using namespace testcontainers;

const modules::NATSContainer nats = modules::NATSImage().start();

nats.host();               // "localhost" for a local daemon
nats.port();               // the published host port for 4222
nats.url();                // "nats://localhost:<port>"
nats.monitoring_url();     // "http://localhost:<mport>" — /healthz, /varz, /connz, ...
```

## Configuration

```cpp
const modules::NATSContainer nats =
    modules::NATSImage()
        .with_username("app")                          // --user/--pass pair
        .with_password("s3cr3t")
        .with_jetstream()                              // -js
        .with_command_args({"--name", "orders-bus"})   // extra nats-server flags
        .start();

// "nats://app:s3cr3t@localhost:<port>"
const std::string url = nats.url();
```

- **`with_username` / `with_password`** come as a pair — half a pair throws at render.
  `url()` gains `user:pass@` (percent-encoded).
- **`with_jetstream()`** enables JetStream. Stream data lives in the container layer — gone
  with the container; `with_reuse` keeps it across runs, or mount a volume over a
  `--store_dir` directory through a customizer.
- **`with_command_arg` / `with_command_args`** append `nats-server` flags after the managed
  ones. NATS reads no environment — the command line *is* the configuration, which also
  makes every option visible to [container reuse](../core-concepts.md#cleanup-raii-ryuk)
  hashing automatically.

## Connecting

Most clients accept the credentials-in-URL form `url()` renders; clients that take the
pieces separately use `host()` / `port()` / `username()` / `password()`. Peer containers on
a shared docker network connect to `<alias>:4222` — the in-container client port, not the
mapped host port.

The monitoring endpoints (`/healthz`, `/varz`, `/connz`, `/jsz`, ...) answer plain
unauthenticated HTTP GETs on `monitoring_url()` — server introspection without a NATS
client library.

## Behavior notes

- The default image is built **FROM scratch**: the server binary is the only file inside,
  so `container().exec(...)` has nothing to run. Assert from the host — the CRLF text
  protocol on `port()` or the monitoring API. The `-alpine` tags add a busybox shell if you
  need one (`with_image("nats:2.12-alpine")`); there is no NATS CLI in either variant.
- The module always owns the container command (`-m 8222` first — overriding the image's
  CMD drops its stock config file, so monitoring is restated unconditionally).
- Flags the module renders throw at render time when passed through `with_command_args`:
  the credentials (`--user`/`--pass`), JetStream (`-js`/`--jetstream`), the monitoring
  listener (`-m`/`--http_port`), the client listener (`-p`/`--port`, `-a`/`--addr`/`--net`),
  and config files (`-c`/`--config`). The server keeps the *last* occurrence of a duplicated
  flag, so a raw copy would silently desync `url()` and the getters or starve the readiness
  probe. Config-file setups take full command ownership via a customizer's `with_cmd`.
- Clustering and leaf nodes are out of scope (multi-container topologies).
