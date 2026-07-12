# ScyllaDB

`modules::ScyllaDBImage` → `modules::ScyllaDBContainer` — pinned **`scylladb/scylla:2026.1`**
(the current LTS line; the tag floats over its patch releases), CQL port 9042, no
authentication (ScyllaDB's own default). The module owns the vendor-recommended CI flags —
`--developer-mode=1 --overprovisioned=1 --smp 1 --memory 512M` — so a fresh node is
CQL-ready in seconds, not minutes. Readiness is an *ordered pair*: the
`Starting listening for CQL clients` log line (printed only after node init, so it gates
out the long boot), then an in-container `cqlsh` query — the authoritative proof that the
query path answers.

!!! note "License"
    The image is **source-available** (ScyllaDB Software License Agreement), not OSI
    open source: free up to 10 TB of storage / 50 vCPUs per organization — a test
    container is orders of magnitude inside — and CI/automated-testing use is explicitly
    permitted for commercial ScyllaDB customers as well. The library links no ScyllaDB
    code; your tests pull the image at run time, the same posture as the `mongo:7` (SSPL)
    module. If your policy needs a pure open-source image:
    `with_image("scylladb/scylla:6.2")` — the last AGPL release, same entrypoint contract.

## Quick start

```cpp
#include "testcontainers/modules/ScyllaDB.hpp"

using namespace testcontainers;

const modules::ScyllaDBContainer scylla = modules::ScyllaDBImage().start();

scylla.host();           // "localhost" for a local daemon
scylla.port();           // published host port for 9042
scylla.contact_point();  // "localhost:<port>" — for host:port-taking clients
scylla.datacenter();     // "datacenter1" — for DC-aware load balancing
```

With the cassandra.h-style C/C++ drivers (ScyllaDB's cpp-rs-driver, the DataStax API):

```cpp
CassCluster* cluster = cass_cluster_new();
cass_cluster_set_contact_points(cluster, scylla.host().c_str());
cass_cluster_set_port(cluster, scylla.port());
// DC-aware balancing, if you configure it explicitly:
cass_cluster_set_load_balance_dc_aware(cluster, scylla.datacenter().c_str(), 0, cass_false);
```

## Configuration

```cpp
const modules::ScyllaDBContainer scylla =
    modules::ScyllaDBImage()
        .with_smp(2)                    // CPU shards (--smp); ~512M memory per shard
        .with_memory("1G")              // total server memory (--memory)
        .with_datacenter("dc-test")     // reported datacenter (--dc)
        .with_command_args({"--rack", "r1"})   // anything else, appended last
        .with_init_script("schema.cql",
                          "CREATE KEYSPACE tc WITH replication = "
                          "{'class': 'NetworkTopologyStrategy', 'replication_factor': 1};")
        .start();
```

- **Flags, not env**: ScyllaDB's docker configuration is entrypoint arguments. The module
  renders its managed flags first and your `with_command_args` after — the entrypoint
  keeps the **last** occurrence, so your duplicate wins (the Kafka rule: tuning knobs, no
  credential getters to desync). Two exceptions: `--dc` belongs to `with_datacenter` (a
  raw duplicate desyncs the getter), and `--authenticator` breaks the authless readiness
  probe — pair it with `with_wait`.
- **`with_init_script`** (host file or in-memory) queues `.cql` scripts the module runs
  through the in-container `cqlsh` **after** the node is ready (ScyllaDB has no
  `initdb.d`), in registration order; a failing script fails `start()`. Keyspace tip:
  newer releases enable *tablets* for fresh keyspaces, which reject `SimpleStrategy` —
  use `NetworkTopologyStrategy` with `'replication_factor': 1`.
- **`exec_cql(cql)`** runs statements through the in-container `cqlsh` against the node's
  own address — zero-dependency seeding and asserts. Output is cqlsh's aligned table:
  assert with substrings (`"(1 rows)"`), not exact matches.
- **No authentication surface** by design: a password on a throwaway localhost container
  buys nothing, and `PasswordAuthenticator` creates its superuser asynchronously *after*
  CQL comes up — test-hostile complexity. Deliberately auth-enabled servers go through
  `with_command_args` + `with_wait`.

## Connecting

`contact_point()` (or `host()` + `port()`) feeds any Cassandra-compatible driver. Peer
containers on a shared docker network dial `<alias>:9042` with the class constant
`ScyllaDBImage::kPort`. Single node only — multi-node clusters would break host-side
driver discovery and are out of scope, like every DB module.

## Behavior notes

- **Startup budget is 120s** by default (not the family 60s): a first boot initializes
  the data directory. Observed cold boots are 5–8s locally in developer mode; the
  headroom is for loaded CI. Init scripts run after readiness and are not under this
  budget (each statement is bounded by cqlsh's own request timeout).
- The shard-aware port 19042 is deliberately not exposed: shard selection there is by
  client source port, which Docker's NAT rewrites — it degrades to 9042 behavior anyway.
  Experiment via `with_customizer` + `with_exposed_port(tcp(19042))`.
- `with_wait` **replaces** the default readiness pair.
- A reused (adopted) node keeps its data; init scripts are not re-run. Editing a script
  changes the reuse hash, so the next `start()` builds a fresh container.
