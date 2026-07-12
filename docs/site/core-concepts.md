# Core concepts

This page is the map of the core API. Every class and method mentioned here is documented in
detail in the [API reference](https://cppudge.github.io/testcontainers-cpp/api/), generated
from the public headers.

## `GenericImage` → `Container`

The core split mirrors "recipe vs running instance":

- **`GenericImage`** is a copyable, reusable **builder**: each `with_*` setter mutates in
  place and returns the builder for chaining. Configure once, `start()` many times.
- **`Container`** is a move-only **RAII handle** to a running container: when it leaves
  scope, the container is force-removed. `keep()` releases that ownership at runtime
  (`keep(false)` re-arms it).

```cpp
GenericImage img = GenericImage("nginx", "1.27")     // or from_reference("nginx:1.27")
                       .with_exposed_port(tcp(80))
                       .with_env("KEY", "value")
                       .with_wait(wait_for::http("/", tcp(80)));

Container web = img.start();   // pull if missing → create → start → wait
```

Anything the typed setters don't cover goes through `with_create_body_patch` — an RFC-7386
deep-merge applied **after** the typed fields, so it wins on conflicts.

## Wait strategies

`start()` returns only when the container is actually ready. Seven strategies live in
`wait_for::`, chainable under one shared startup timeout (`with_startup_timeout`):

| Strategy | Ready when |
|---|---|
| `stdout_message` / `stderr_message` / `log` | the message appeared in the logs *n* times |
| `seconds` / `millis` | the fixed duration elapsed |
| `exit` / `exit_code` | the container exited (optionally with a specific code) |
| `healthy` | the image's own Docker healthcheck reports healthy |
| `http(path, port, status)` | an HTTP probe of the **published host port** answers |
| `listening_port(port)` | a TCP connect to the published host port succeeds |
| `successful_command` / `successful_shell_command` | an in-container exec exits 0 |

```cpp
GenericImage("postgres", "16-alpine")
    .with_exposed_port(tcp(5432))
    .with_wait(wait_for::log("database system is ready to accept connections"))
    .with_wait(wait_for::listening_port(tcp(5432)))   // strategies run in order
    .with_startup_timeout(std::chrono::seconds(60));
```

Two caveats worth knowing: `listening_port` probes the **host** side of the mapping only (a
port published before the process binds can read ready early — prefer a log or command wait
for such images), and `successful_command` probes should be short-lived — an attempt still
running at the deadline is abandoned client-side but keeps running in the container (Docker
has no exec-kill API).

## Ports and the container host

Declare container ports with `with_exposed_port(tcp(...) / udp(...))`; Docker publishes each
onto an ephemeral host port, discovered via `get_host_port` (IPv4-preferred; `_ipv4`/`_ipv6`
variants exist).

`Container::host()` returns the address to reach those published ports: an explicit
`TESTCONTAINERS_HOST_OVERRIDE` / `host.override` wins; a `tcp://`/`https://` daemon yields its
hostname; a local socket/pipe yields `localhost` — unless the test process itself runs inside
a Linux container (Docker-in-Docker), where the docker bridge gateway is returned instead.
You never hard-code an address; `host()` + `get_host_port()` are correct in every topology.

## Cleanup: RAII + Ryuk {#cleanup-raii-ryuk}

Cleanup is layered:

1. **RAII** — dropping the `Container`/`Network`/`Volume` handle force-removes the resource.
2. **Ryuk** — a per-daemon reaper container watches a session label on everything the library
   creates (containers, networks, volumes, built images, compose projects) and sweeps it all
   a few seconds after the test process dies — even on a crash or `SIGKILL`. A daemon where
   Ryuk cannot start fails the run loudly rather than running unwatched; opt out explicitly
   with `TESTCONTAINERS_RYUK_DISABLED=true` (Ryuk is also skipped on Windows-containers
   engines, where no Linux reaper image can run).

**Reuse** (`with_reuse`) opts a container out of both layers: gated globally on
`TESTCONTAINERS_REUSE_ENABLE`, `start()` then adopts an already-running container whose
config hash matches instead of creating a new one — turning the per-test container into a
per-machine one for fast local iteration. Reused containers are never auto-removed; prune
them externally by the `org.testcontainers.reuse.hash` label.

## Exec, logs, copy

- **`exec(cmd[, opts])`** runs a command in the running container and returns
  `ExecResult{stdout_data, stderr_data, exit_code}`. `ExecOptions` covers env, working dir,
  user, tty, stdin, detach; streaming overloads deliver output incrementally, optionally
  under an absolute deadline.
- **`logs()`** snapshots the log; **`follow_logs(consumer)`** streams incrementally
  (blocking — run it on your own thread; the consumer returns `false` to stop).
- **Copy** — `copy_to()` ships host files, in-memory bytes, or whole directories (as a
  streamed tar; also available pre-start via `GenericImage::with_copy_to`); `read_file` /
  `copy_file_from` bring single files back, `DockerClient::copy_from_container_to` whole
  trees.

## Networks and volumes

```cpp
Network net = Network::builder().with_driver("bridge").create();

Container a = GenericImage("redis", "7.2")
                  .with_network(net)
                  .with_network_alias("cache")     // DNS name for peers on `net`
                  .start();
// peers on `net` reach it at cache:6379 — no published ports needed
```

`Volume` is the same shape (`Volume::create()` / `builder()`), mountable via
`Mount::volume(v.name(), "/target")` and seedable with `populate()` — which works on Windows
volumes too. Both handles are RAII + session-labeled, so Ryuk covers crash paths.

## Building images

`GenericBuildableImage` builds from a Dockerfile (host path or inline string) plus a context
assembled from host files/dirs and in-memory data — streamed to the daemon, honoring
`.dockerignore` with docker-build semantics:

```cpp
GenericImage app = GenericBuildableImage("my-app", "test")
                       .with_dockerfile_string("FROM alpine:3.20\nCOPY run.sh /run.sh\n...")
                       .with_file("/host/path/run.sh", "run.sh")   // into the build context
                       .build();          // throws on build failure, with the failing step's output
Container c = app.with_exposed_port(tcp(8080)).start();
```

Built images are session-scoped (Ryuk removes them after the process exits); base images and
pulled layers are untouched. `GenericImage::exists(name, tag)` is the local-presence probe
for skip-if-built flows.

## Docker Compose

`DockerComposeContainer` drives a whole `docker compose` stack — the one documented exception
to the "no docker CLI" rule, with three client modes: **local** (host `docker compose` CLI),
**containerised** (a `docker:26.1-cli` container with the daemon socket mounted — no host CLI
needed), and **auto** (probe local, fall back).

```cpp
auto stack = DockerComposeContainer::with_auto_client({"docker-compose.yml"})
                 .with_exposed_service("db", tcp(5432));  // TCP-probed at up
stack.start();
const std::uint16_t port = stack.get_service_port("db", tcp(5432));
```

Scaling (`with_scale` + per-instance getters), per-service logs, profiles, and a socat
ambassador for unpublished ports are covered in the API reference. Teardown is RAII
(`down` + a label sweep), and the stack's compose label is registered with Ryuk before `up`,
so crashed runs are swept too.

## Errors

Errors are thrown, not returned:

- **`DockerError`** — the base: carries the HTTP `status_code()` (empty for transport
  failures) and a best-effort `resource_id()`.
- **`NotFoundError`** — every 404 site. One caveat: registries answer 404 for *private*
  images requested without credentials, so a pull `NotFoundError` can also mean
  "authentication required".
- **`TransportTimeoutError`** — an I/O deadline expired (derives `DockerError`).
- **`StartupTimeoutError`** — the wait strategy did not pass in time. Deliberately derives
  `Error`, **not** `DockerError`: your app not becoming ready is not a daemon failure.

## Several daemons at once

Every operation has a form taking an explicit `DockerClient`, so one test process can drive
several daemons (e.g. a local one plus a remote TLS engine). Reapers are per-daemon — each
endpoint gets its own Ryuk, and every daemon's resources are crash-swept independently.
