# OpenSearch

`modules::OpenSearchImage` → `modules::OpenSearchContainer` — pinned
**`opensearchproject/opensearch:3.7.0`** (the active 3.x line; the hub publishes no
minor-line tags, so the pin is patch-level), REST port 9200, **security plugin disabled**:
plain HTTP, no credentials — `http_url()` is all a REST client needs. Readiness is HTTP 200
from `/_cluster/health` through the published port: the root endpoint answers from the
local node, while cluster health answers only once a cluster manager is elected — what the
first index or search call actually needs.

## Quick start

```cpp
#include "testcontainers/modules/OpenSearch.hpp"

using namespace testcontainers;

const modules::OpenSearchContainer search = modules::OpenSearchImage().start();

search.host();      // "localhost" for a local daemon
search.port();      // published host port for 9200
search.http_url();  // "http://localhost:<port>" — point any HTTP client at it
```

Seeding and asserts work driver-free through the in-image `curl`:

```cpp
search.container().exec({"curl", "-s", "-XPUT",
                         "http://localhost:9200/idx/_doc/1?refresh=true",
                         "-H", "Content-Type: application/json",
                         "-d", R"({"msg":"hello"})"});
// ?refresh=true makes the document searchable immediately.
```

## Configuration

```cpp
const modules::OpenSearchContainer search =
    modules::OpenSearchImage()
        .with_env("cluster.name", "it-cluster")               // dotted key = a setting
        .with_env("OPENSEARCH_JAVA_OPTS", "-Xms1g -Xmx1g")    // your duplicate wins
        .start();
```

- The module manages four env keys: `discovery.type=single-node` (forms a one-node
  cluster and bypasses the bootstrap checks — no `vm.max_map_count` sysctl needed),
  `DISABLE_SECURITY_PLUGIN=true` + `DISABLE_INSTALL_DEMO_CONFIG=true` (plain HTTP, no
  demo certs/passwords), and a 512 MB heap (`OPENSEARCH_JAVA_OPTS` — the image default
  is 1 GB). Your `with_env` entries land **after** them, so on a duplicate key *you*
  win — these are engine tuning, not credential mirrors, and overriding the heap or
  discovery is fair game.
- Keys shaped like `section.setting` become real OpenSearch settings — the entrypoint
  turns them into `-E` options, so `with_env` *is* the settings API.
- **Re-enabling security** flips 9200 to https with self-signed demo certs, which the
  plain-HTTP readiness probe can never pass. Replace it via `with_wait` then — the
  working recipe is
  `wait_for::successful_shell_command("curl -sk --fail -u admin:<password> https://localhost:9200/")`.
  A typed secure variant is a planned follow-up.

## Connecting

`http_url()` feeds any HTTP client or opensearch-cpp; there are no credentials to pass.
Peer containers on a shared docker network dial `http://<alias>:9200`. No accounts exist
in this mode, so there are deliberately no `username()`/`password()` getters — the secure
follow-up will introduce them with real values.

## Behavior notes

- **Startup budget is 120s** by default (not the family 60s): a JVM engine booting a
  gigabyte of plugins takes 10–20s warm and more on loaded CI.
- Right after `start()` the cluster status may still be **yellow** (an internal shard
  initializing) — the probe's promise is that the health endpoint *answers*, which is
  what index/search calls need; don't assert `green` in your own tests.
- Only 9200 is published. The performance-analyzer ports (9600/9650) and the transport
  port (9300) stay unpublished — reach them via
  `with_customizer([](GenericImage& g) { g.with_exposed_port(tcp(9600)); })`.
- A reused (adopted) server keeps its indices; a changed env (say, the heap) changes the
  reuse hash and builds a fresh container.
