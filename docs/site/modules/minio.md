# MinIO

`modules::MinIOImage` → `modules::MinIOContainer` — pinned
**`minio/minio:RELEASE.2025-09-07T16-13-09Z`**, S3 API port 9000 + web-console port 9001,
credentials **minioadmin/minioadmin**. The module owns the command
`server /data --console-address :9001` (without the fixed console address the server picks
a random console port every boot, which could not be exposed). Readiness is HTTP 200 from
`/minio/health/cluster` through the published port — the one health endpoint gated on the
object layer being *writable* (`/live` and `/ready` answer as soon as the process serves
HTTP).

!!! note "The pin is frozen"
    Upstream stopped publishing community Docker images in late 2025 and put the community
    edition in maintenance mode — this tag is the **final** community image and will not be
    bumped. It predates the source-only fix for CVE-2025-62506 (a privilege-escalation
    issue requiring valid credentials — not a practical concern for a throwaway test server
    on ephemeral localhost ports). If your policy needs a patched build, point
    `with_image()` at a maintained rebuild; the command/env contract is unchanged. A
    `-cpuv1` twin of the pin exists for old x86-64 CPUs. See also the API-compatible
    [RustFS module](rustfs.md).

## Quick start

```cpp
#include "testcontainers/modules/MinIO.hpp"

using namespace testcontainers;

const modules::MinIOContainer minio = modules::MinIOImage()
                                          .with_bucket("test-data")
                                          .start();

minio.host();          // "localhost" for a local daemon
minio.s3_port();       // published host port for 9000
minio.s3_url();        // "http://localhost:<port>" — the S3 endpoint override
minio.access_key();    // "minioadmin"
minio.secret_key();    // "minioadmin"
minio.console_url();   // the web console (object browser at this release)
```

With the AWS SDK for C++ — endpoint override plus **path-style addressing** (the bucket
rides in the URL path; virtual-host style needs wildcard DNS a test container does not
have):

```cpp
Aws::Client::ClientConfiguration cfg;
cfg.endpointOverride = minio.s3_url();
cfg.scheme = Aws::Http::Scheme::HTTP;
Aws::S3::S3Client s3(
    Aws::Auth::AWSCredentials(minio.access_key(), minio.secret_key()), cfg,
    Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
    /*useVirtualAddressing=*/false);
```

## Configuration

```cpp
const modules::MinIOContainer minio =
    modules::MinIOImage()
        .with_access_key("tc-admin")        // >= 3 chars (the server's rule)
        .with_secret_key("super-secret-8")  // >= 8 chars (ditto)
        .with_bucket("raw")
        .with_bucket("processed")
        .start();
```

- **Credentials** render as `MINIO_ROOT_USER` / `MINIO_ROOT_PASSWORD`, appended after your
  `with_env` entries — the module wins on duplicates, so the getters can never disagree
  with the server. The server's own length rules (access key ≥ 3, secret key ≥ 8) are
  enforced at `start()`, before any daemon contact — also why this module defaults to the
  image's minioadmin pair instead of the house "test".
- **`with_bucket`** creates buckets right after readiness via the in-image `mc` client
  (`mc mb --ignore-existing`). The hook first aliases the container's own `mc` to the
  server as **`tc`** — your follow-up `container().exec({"mc", ...})` calls can use
  `tc/<bucket>` paths directly. The bucket list also renders into a
  `org.testcontainers.minio.buckets` label (sorted), so container reuse notices a changed
  list.
- **`with_env`** reaches the server's other knobs: `MINIO_REGION`, `MINIO_DOMAIN`,
  `MINIO_BROWSER=off` (disables the console), ... The legacy `MINIO_ACCESS_KEY` /
  `MINIO_SECRET_KEY` spellings are ignored by the pinned server — use the typed setters.

## Connecting

`s3_url()` is the endpoint for clients in the test process. Peer containers on a shared
docker network dial `http://<alias>:9000` with the same credentials. One S3-specific
caveat: **presigned URLs embed the endpoint they were generated against** — generate them
against the address their consumer will dial (host-side vs alias).

## Behavior notes

- `with_wait` **replaces** the default health probe (for TLS setups — the default probe
  speaks plain HTTP only).
- The web console at this release is an object **browser** only (community administration
  moved to `mc`); it pairs well with `container().keep()` when eyeballing a failed test's
  buckets.
- Replacing the command via `with_customizer` drops the fixed console address —
  `console_url()` then points at nothing.
- Teardown removes the image's anonymous `/data` volume with the container. A reused
  (adopted) server keeps its object store and buckets — hooks do not re-run on adoption;
  a changed bucket list changes the reuse label, so the next `start()` builds a fresh
  container instead.
