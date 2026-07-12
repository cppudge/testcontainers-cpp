# Mosquitto (MQTT)

`modules::MosquittoImage` → `modules::MosquittoContainer` — pinned **`eclipse-mosquitto:2.0`**,
port 1883. The module ships a managed `mosquitto.conf` (`listener 1883` +
`allow_anonymous true`) into the container before start — without it, a configless
Mosquitto 2.x listens on the *container's loopback only* and the mapped port connects to
nothing. Readiness is the broker's own "running" log line, printed after the listening
sockets are open.

## Quick start

```cpp
#include "testcontainers/modules/Mosquitto.hpp"

using namespace testcontainers;

const modules::MosquittoContainer broker = modules::MosquittoImage().start();

broker.host();          // "localhost" for a local daemon
broker.port();          // the published host port for 1883
broker.broker_url();    // "tcp://localhost:<port>" — the Paho serverURI form
```

## Configuration

```cpp
const modules::MosquittoContainer broker =
    modules::MosquittoImage()
        .with_config_option("persistence", "true")   // appended to the managed config
        .start();

// Or replace the whole config and own the contract yourself:
const modules::MosquittoContainer locked =
    modules::MosquittoImage()
        .with_config_content("listener 1883\nallow_anonymous false\n")
        .start();
```

- **`with_config_option(key, value)`** appends `<key> <value>` lines after the managed
  listener block, in call order — the right spot for both global and listener-scoped
  options (they bind to the module's port-1883 listener).
- **`with_config(path)` / `with_config_content(bytes)`** *replace* the managed config
  entirely (repeat calls replace again; last wins). You then own the whole contract: keep a
  `listener 1883`, keep startup logs on stdout/stderr, and pick your own auth
  (`allow_anonymous` defaults to **false** once a listener is defined).
- Combining a replacement config with `with_config_option` **throws at render**: mosquitto
  options are order- and listener-scoped, so merging has no well-defined meaning.

## Connecting

`broker_url()` renders the `tcp://host:port` serverURI the Eclipse Paho C/C++ clients take
(the identical `mqtt://` spelling works there too); libmosquitto and `mosquitto_pub`/`_sub`
take `host()` / `port()` directly. Peer containers on a shared docker network connect to
`<alias>:1883`.

The image ships `mosquitto_pub` / `mosquitto_sub`, so behavioral assertions can run
in-container — retained messages make it race-free:

```cpp
broker.container().exec({"mosquitto_pub", "-t", "greet", "-m", "hello", "-r"});
const ExecResult r = broker.container().exec(
    {"mosquitto_sub", "-t", "greet", "-C", "1", "-W", "5"});
// r.stdout_data == "hello\n"
```

## Behavior notes

- The module manages exactly one thing: the config file at
  `/mosquitto/config/mosquitto.conf`. No env keys (the image reads none), no command
  override.
- Authentication is deliberately not surfaced yet: the official (TLS-compiled) broker
  requires *hashed* `password_file` entries, and hashing client-side would drag in an
  OpenSSL dependency. Today: pass a config naming a `password_file` and copy a pre-hashed
  file in through a customizer's `with_copy_to`.
- Websockets (conventionally port 9001) are config-opt-in:
  `with_config_option("listener", "9001")` + `with_config_option("protocol", "websockets")`
  + a customizer's `with_exposed_port(tcp(9001))`.
- An adopted (reused) broker keeps its retained messages and persistent sessions.
