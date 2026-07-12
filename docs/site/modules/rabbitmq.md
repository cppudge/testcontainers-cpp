# RabbitMQ

`modules::RabbitMQImage` → `modules::RabbitMQContainer` — pinned
**`rabbitmq:3.13-management`**, AMQP port 5672 + management HTTP 15672, default credentials
`guest`/`guest`, vhost `/`.

The management variant is deliberate: its HTTP API is the one broker-inspection surface a
no-drivers test can hit, and the weight argument is a myth — the tag differs from the plain
image essentially by `enabled_plugins`, not a fat layer.

## Quick start

```cpp
#include "testcontainers/modules/RabbitMQ.hpp"

using namespace testcontainers;

const modules::RabbitMQContainer mq =
    modules::RabbitMQImage()
        .with_username("app")
        .with_password("s3cr3t")
        .start();

mq.amqp_url();          // "amqp://app:s3cr3t@localhost:<port>"
mq.management_url();    // "http://localhost:<mgmt port>"
mq.management_port();   // for HTTP API calls
```

`amqp_url()` emits **no path** for the default vhost `/` — an absent path means "the
client's default vhost" in every mainstream client, and unlike the spec-equivalent `/%2F`
it survives URI parsers that skip percent-decoding. Any other vhost renders percent-encoded
as one segment.

## Configuration

- **`with_username` / `with_password` / `with_vhost`** — the provisioned account.
  `with_username` *replaces* the built-in `guest` account (image contract). An empty
  password fails fast at render — the broker's internal auth backend prohibits
  blank-password logins outright.
- **`with_definitions(host_json)` / `with_definitions_json(json)`** — bulk topology import
  (queues, exchanges, bindings, users, …) via RabbitMQ's own definitions mechanism. The
  module imports a *directory*: a synthesized seed file carrying your configured
  user/password/vhost goes first, your files after it (call order) — so definitions
  **compose with the credential setters** instead of silently disabling them (under a plain
  `load_definitions` file RabbitMQ skips *all* default provisioning: a definitions file
  with no `users` entry would leave the broker with zero users, `guest` included).
- **`with_plugin(name)`** — enabled post-ready via `rabbitmq-plugins enable` (additive — the
  image's own management/prometheus plugins stay), with an order-normalized reuse label so a
  changed plugin set creates a fresh container.

## Behavior notes

- Readiness is **ordered** and the order is load-bearing: the "Server startup complete" log
  wait runs *first*, one `rabbitmq-diagnostics check_port_connectivity` exec second. The
  image has no `USER` directive, so execs run as root — and any Erlang CLI in the first
  seconds of boot would create a root-owned `.erlang.cookie` that the uid-999 server then
  cannot read, killing the node unrecoverably. A log wait never execs; by the time the
  diagnostics probe runs, the server long since wrote its own cookie.
- Remote `guest` logins work only by the official image's `loopback_users.guest = false`
  grace — custom users (`with_username`) sidestep that on hardened base images.
- Out of scope: clustering, TLS/amqps, MQTT/STOMP typed getters, and a per-object topology
  builder API (definitions import is RabbitMQ's own bulk mechanism for that).
