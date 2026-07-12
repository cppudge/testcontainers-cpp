# RustFS

`modules::RustFSImage` → `modules::RustFSContainer` — pinned **`rustfs/rustfs:1.0.0-beta.8`**,
S3 API port 9000 + web-console port 9001, credentials **rustfsadmin/rustfsadmin**. RustFS
is an S3-compatible object store in Rust and a natural stand-in for MinIO now that MinIO's
community image line is frozen; the image boots commandless (its entrypoint launches the
server against the image-declared `/data` volume). Readiness is HTTP 200 from `/health` on
the S3 port — the server's own health endpoint, reporting the storage/IAM/lock subsystems
ready.

!!! note "Beta pin"
    RustFS has no stable release line yet — the pin is the newest upstream beta and moves
    with upstream releases until 1.0.0 lands (upstream itself warns against production use;
    a test container is exactly the right blast radius). Every release also ships a
    `-glibc` twin of its Alpine/musl default tag; both work with the module via
    `with_image()`.

## Quick start

```cpp
#include "testcontainers/modules/RustFS.hpp"

using namespace testcontainers;

const modules::RustFSContainer rustfs = modules::RustFSImage().start();

rustfs.host();          // "localhost" for a local daemon
rustfs.s3_port();       // published host port for 9000
rustfs.s3_url();        // "http://localhost:<port>" — the S3 endpoint override
rustfs.access_key();    // "rustfsadmin"
rustfs.secret_key();    // "rustfsadmin"
rustfs.console_url();   // "http://localhost:<port>/rustfs/console/" — the web UI
```

The getter vocabulary is deliberately identical to the [MinIO module](minio.md)'s —
swapping one image class for the other is the whole migration. With the AWS SDK for C++,
configure the endpoint override plus **path-style addressing** exactly as on the MinIO
page.

## Configuration

```cpp
const modules::RustFSContainer rustfs =
    modules::RustFSImage()
        .with_access_key("testkey")
        .with_secret_key("testsecret1234")
        .start();
```

- **Credentials** render as `RUSTFS_ACCESS_KEY` / `RUSTFS_SECRET_KEY`, appended after your
  `with_env` entries — the module wins on duplicates, so the getters can never disagree
  with the server. Only emptiness throws at `start()`: the server imposes no documented
  length rules.
- **`with_env`** reaches the image's other knobs: `RUSTFS_CONSOLE_ENABLE=false`,
  `RUSTFS_OBS_LOGGER_LEVEL=info`, ...
- No `with_bucket` here: the image ships no S3 CLI to hook (it does ship `curl` and a
  shell for `container().exec()` probing), and an SDK client creates buckets in one call.

## Connecting

`s3_url()` is the endpoint for clients in the test process. Peer containers on a shared
docker network dial `http://<alias>:9000` with the same credentials and path-style
addressing. Presigned URLs embed the endpoint they were generated against — generate them
against the address their consumer will dial.

## Behavior notes

- `with_wait` **replaces** the default `/health` probe (for TLS setups — the default probe
  speaks plain HTTP only).
- The web UI lives under **`/rustfs/console/`** — the console port answers 403 at the bare
  root, which reads like a failure to a human pasting `http://localhost:<port>`;
  `console_url()` includes the prefix.
- Teardown removes the image's anonymous `/data` volume with the container; a reused
  (adopted) server keeps its buckets and objects.
