# Kafka

`modules::KafkaImage` → `modules::KafkaContainer` — a **single-node KRaft** broker (no
ZooKeeper), pinned **`apache/kafka:3.9.1`** (the official ASF image: small, ships the CLI
tools, and the last 3.x line for maximum client-protocol compatibility).

Kafka cannot be "env + port + wait": the broker must *advertise* an address, the host-side
address contains the mapped port — which does not exist until after start — and listeners
are read once, at boot. The module runs the classic Testcontainers two-phase boot for you
(placeholder command → resolve the real host/port → write the starter script → follow the
logs until "Kafka Server started"), so from the outside it is still just `start()`.

## Quick start

```cpp
#include "testcontainers/modules/Kafka.hpp"

using namespace testcontainers;

const modules::KafkaContainer kafka =
    modules::KafkaImage()
        .with_topic("events", /*partitions=*/3)   // pre-created before start() returns
        .start();

kafka.bootstrap_servers();   // "localhost:<port>" — for clients on the host
```

`bootstrap_servers()` returns bare `host:port` — no `PLAINTEXT://` scheme, which librdkafka
would reject.

## Two bootstrap addresses

| Getter | For | Value |
|---|---|---|
| `bootstrap_servers()` | clients on the **host** | `host:<published 9092>` |
| `internal_bootstrap_servers()` | clients on the **same docker network** | `<first network alias>:9093` |

!!! warning "In-container clients must use `:9093`"
    Kafka's metadata reply carries the advertised address of the listener the connection
    arrived on. The `:9092` listener advertises the *host-side* address — unreachable from
    inside the network. A containerised client bootstrapping `:9092` connects, gets metadata
    pointing back at the host, and hangs. Use `internal_bootstrap_servers()` (give the broker
    a `with_network_alias` first).

## Configuration

- **`with_topic(name, partitions = 1)`** — pre-creates topics before `start()` returns (and
  renders a reuse-visible label, so a changed topic set creates a fresh container instead of
  adopting one without it).
- **`with_cluster_id(id)`** — the KRaft cluster id (22 URL-safe base64 characters, validated
  at render). The default is fixed, keeping [reuse](../core-concepts.md#cleanup-raii-ryuk)
  hashes deterministic.
- **`with_env`** — broker tuning (`KAFKA_*`). Unlike the database modules, *your* entries
  land after the module's, so **you win** on duplicates.
- `with_startup_timeout` budgets **each** of the two phases — worst case ≈ 2×.

## Behavior notes

- Readiness is log-based, not exec-based, on purpose: a command probe would spawn a JVM per
  poll and break `apache/kafka-native` image overrides.
- Confluent images (`confluentinc/cp-kafka`) are untested best-effort: the starter script's
  fallback covers the boot only — `with_topic` still execs the Apache CLI path.
- Out of scope: SASL/TLS listeners, multi-broker quorums, Schema Registry / Kafka Connect
  (natural separate modules, consuming `internal_bootstrap_servers()`).
