# Feature notes — what is implemented, and its known limits

Short reference notes on the implemented subsystems, distilled from the backlog as items were
completed. This file records what EXISTS, where it lives, and the caveats a user or maintainer
should know; the actionable leftovers live in [TODO.md](../TODO.md).

## Transport & client

**Transports** (`src/docker/Transport.*`) — unix socket / Windows named pipe / TCP / TLS behind
one `ITransport`; `connect()` picks by scheme. TLS materials resolve from `DOCKER_CERT_PATH`
(falling back to `~/.docker` under `DOCKER_TLS_VERIFY`) via the pure `TlsConfig` helpers
(unit-tested). End-to-end TLS against a real remote daemon is not CI-verified yet (see TODO).

**I/O deadlines** (`docker::TransportTimeouts`) — `connect` budgets the whole establishment
(resolve + connect + TLS handshake, default 10s); `io` deadlines each read/write (default 60s,
`nullopt` disables). Expiry throws `TransportTimeoutError` and the connection is discarded.
Long-poll endpoints widen the io deadline (`stop` past its grace period, `build` to 10 min);
the streaming call sites (`follow_logs`, exec attach) disable it after the response header.
Implementation constraints worth knowing: cancelling a multi-endpoint `asio::async_connect`
does NOT abort it — deadline expiry must `close()` the socket, not `cancel()` it; and
`resolver.cancel()` cannot interrupt an in-flight getaddrinfo, so the connect budget is soft
for the DNS-resolve leg of non-literal hosts.

**Connection model** — connection-per-request by default; `DockerClient::Session` (RAII, per
instance) opts consecutive idempotent GETs into reusing one kept-alive connection — used by the
wait-strategy polling loop. A stale reused connection (died while idle) is retried ONCE on a
fresh one; a timeout is never retried; non-GET requests never reuse, so a retry can never
replay a side effect. **Decision record (2026-07-04, from reading the references):**
testcontainers-rs/bollard deliberately pools nothing (`pool_max_idle_per_host(0)` on every
connector) — connection-per-request is the reference-validated correctness-first default;
testcontainers-java runs an unbounded shared HttpClient5 pool with stale validation off
(`validateAfterInactivity=-1`), which is the source of its podman stale-socket breakage
(testcontainers-java#7310) and idle-fd growth (moby#45539). dockerd itself never idle-closes
(only `ReadHeaderTimeout` is set), but intermediaries (Docker Desktop proxy, NAT, podman's ~5s
idle close) do — hence GET-only reuse + retry-once. A full shared pool is deferred (TODO,
"option B"). Residual: a Session makes that one instance non-thread-safe while active; copies
stay independent.

**Docker host resolution** (`DockerHost::resolve`, `src/docker/HostResolve.hpp`) — the
testcontainers order, first hit wins: `DOCKER_HOST` → `docker.host` in
`~/.testcontainers.properties` → active docker context (`DOCKER_CONTEXT` / `currentContext`) →
platform default (rootless socket fallbacks on Linux; named pipe on Windows). Steps 2–4 never
throw on a malformed file. Only the endpoint is consumed: docker-context TLS materials and
other properties keys are not read (see TODO).

**Error model** (`include/testcontainers/Error.hpp`) — `DockerError` carries `status_code()`
(nullopt for transport failures) and `resource_id()` (best-effort; names the primary resource).
Subtypes: `NotFoundError` (every 404 site), `TransportTimeoutError` (derives DockerError), and
`StartupTimeoutError` — deliberately derives `Error`, NOT `DockerError` (app readiness is not a
daemon failure), carrying the container id / compose service. All JSON parse entry points are
guarded (`guard_parse`: operation name + 2 KiB body excerpt). Caveat: a pull `NotFoundError`
can also mean "authentication required" — registries answer 404 for private images requested
without credentials.

## Container lifecycle

**`ContainerRequest` + `Runner` split** (`include/testcontainers/ContainerRequest.hpp`,
`src/Runner.*`) — the whole orchestration (reuse lookup → retry loop → create → copy-to →
hooks → start → wait → handle) lives in `detail::Runner`; `GenericImage::start()` is
`run(to_request())`. Public `run(request)` / `run(client, request)` bootstrap the Reaper and
delegate; the core skips reaper bootstrap so unit tests drive it against a canned responder.
A hand-built request owns the consistency of the port trio (typed `exposed_ports` vs rendered
`spec.exposed_ports` vs `publish_all_ports`). Modules (Postgres/Redis/…) remain planned as
composition over `GenericImage`.

**Lifecycle hooks + startup retry** — `with_created/starting/started/stopping_hook`
(`LifecycleHook = std::function<void(DockerClient&, const std::string& id)>`) and
`with_startup_attempts(n)` (the whole create→start→wait retried, each failed partial removed;
the reuse-adopt path is not retried; no inter-attempt backoff). A throwing created/starting/
started hook aborts start() and cleans up; stopping fires once on teardown — never for a
persistent (reuse) handle — and its exceptions are swallowed.

**Reaper (Ryuk)** — containers, networks, and named volumes carry the session label; images do
NOT (unreaped). Pinned to `testcontainers/ryuk:0.11.0`, skipped on the Windows engine and via
`TESTCONTAINERS_RYUK_DISABLED`. Process-global: it binds to the FIRST daemon it starts against;
`run(client, request)` boots it on that client's daemon, but later runs against a different
daemon in the same process get labels and no crash-safe reaping (see TODO).

**Reuse** (`with_reuse`) — adopts an already-running container matching a stable FNV-1a
reuse-hash label (`org.testcontainers.reuse.hash`, over the create body + copy-to
descriptors); gated on `TESTCONTAINERS_REUSE_ENABLE` / `testcontainers.reuse.enable`, degrading
to a normal container otherwise. Host-FILE copy sources hash the PATH, not the bytes — a
changed file at the same path still reuses. Reused containers are never auto-removed or reaped:
prune externally (label sweep on the reuse-hash label).

**Wait strategies** — log message / fixed duration / exit(+code) / healthcheck / HTTP probe /
listening port, run in order under one shared startup timeout; the whole polling loop runs
inside a `DockerClient::Session`. `wait_for::listening_port` probes the published HOST port
only (no in-container listen check), so a port published before the process binds could read
ready early. HTTP/TCP probes are deadline-bounded per probe (min(time left, 5s), which absorbs
Windows' ~2s refused-SYN retry on the dead `::1` half of "localhost").

## Container features

**Port / inspect getters** — `get_host_port` (IPv4-preferred), `get_host_port_ipv4/_ipv6`,
`first_mapped_port()` (the first DECLARED port for handles from `start()`; lowest published
otherwise), `inspect()` / `inspect_raw()`. Uncached: every getter re-inspects the container.

**Logs** — `logs()` (snapshot) / `follow_logs()` (incremental streaming; BLOCKING with
cooperative stop via the consumer — run it on your own thread). TTY containers
(`with_tty`) use a raw/unframed decode path, selected automatically by `Container`; a
pseudo-TTY rewrites `\n` → `\r\n`, so match substrings, not exact lines.

**Exec** — `ExecOptions` (env / working_dir / user / privileged / tty / stdin_data) plus a
streaming overload (`exec(cmd, opts, consumer)`). Exec-with-stdin requests a connection
upgrade (`Upgrade: tcp` → HTTP 101, the exec stream then arrives raw, not as an HTTP body) and
feeds stdin only AFTER the response header — both required through Docker Desktop's named-pipe
proxy, which drops client bytes sent after the POST on a non-upgraded connection. Stdin EOF:
TCP / unix sockets half-close via `shutdown(send)`; the Windows named pipe mirrors go-winio's
`CloseWrite` (flush, then a zero-length message — message-mode pipes only, which is what every
real daemon serves); TLS cannot half-close, so exec-with-stdin throws up front there (Go's
`tls.Conn` cannot either — the docker CLI hangs where we throw).

**Copy to / from container** — `CopyToContainer` (host file or in-memory bytes, `with_mode`)
is PUT as a single-entry USTAR tar per source (entry path ≤ 100/255 chars); the target's parent
directory must already exist. Copy-from covers single regular files (`read_file` /
`copy_file_from`); for trees, run `docker::extract_tar` on the raw `copy_from_container` bytes.

**Build from Dockerfile** (`GenericBuildableImage`) — Dockerfile from a host path or an inline
string; context from host files/dirs (recursive walk, no `.dockerignore` filtering) and
in-memory data; `build()` returns a runnable `GenericImage`. Build output is buffered (no live
streaming); built images carry no session label (not reaped). `forcerm=1` is always sent —
without it the legacy builder keeps a failed step's intermediate container, which carries no
labels and leaks past Ryuk.

**HostConfig** — typed setters for memory / shm_size / ulimits / cap_add / cap_drop /
extra_hosts; everything else through `with_create_body_patch` (an RFC-7386 deep-merge applied
AFTER the typed fields, so it overrides them; nest HostConfig fields under `"HostConfig"`).

**Networks** — `Network` RAII handle + builder (driver / internal / attachable / IPv6 / one
IPAM subnet+gateway pair / driver options / labels); `with_network_alias` (per-network DNS
aliases); `Network::connect` attaches a running container. No network inspect and no
process-wide dedup — every `create()` makes a brand-new network.

**Named volumes** — `Volume` RAII handle + builder, session-labeled for Ryuk;
`populate(sources)` seeds data through a throwaway helper container (`alpine:3.20` by default,
started before the archive PUT — not every daemon materializes writes on a created-only
container's mountpoint). No list/prune; the RAII drop fails (409) while a container still
mounts the volume — tear down in reverse-declaration order.

**Image pull policy + name substitution** — `ImagePullPolicy::Default` (lazy: pull on a create
404) / `Always` (pull before every create); substitution via
`TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX` or a custom `with_image_name_substitutor` (replaces the
default). Applied at the `GenericImage` layer only — not for builds, compose, or raw
`DockerClient` calls.

**Registry auth** — explicit `with_registry_auth` or auto-resolved from the Docker config
(`DOCKER_AUTH_CONFIG` → `$DOCKER_CONFIG/config.json` → `~/.docker/config.json`), including
credential helpers (`credsStore` / `credHelpers`, shelling out to
`docker-credential-<helper> get`; plaintext `auths` take precedence). Helper output is not
cached — re-invoked on every pull.

## Compose & Windows

**Docker Compose** (`DockerComposeContainer`) — three client modes: Local (DEFAULT — shells out
to the host `docker compose` CLI, the one documented exception to the library's "no docker CLI"
rule), Containerised (a long-lived `docker:26.1-cli` ambassador with the host socket
bind-mounted), and Auto (probe, then fall back). Readiness = compose v2 `--wait
--wait-timeout` PLUS a TCP probe per `with_exposed_service` (phases budgeted separately).
Local-mode env vars are saved/applied/restored around the child process only. Compose
containers carry no session label (not Ryuk-reaped).

**Windows containers** (docs/04) — engine-mode detection (`server_os()` /
`is_windows_engine()`, cached process-wide), free-form `with_platform`, Ryuk skipped on the
Windows engine (RAII + AutoRemove only — testcontainers-dotnet parity). copy-to still
Unix-normalizes entry paths, so `C:\...` targets are unhandled. The nanoserver test image tag
is host-build-locked (`ltsc2025` on build 26100).

**TTY containers** — `with_tty()` sets `Tty=true`; `logs()` / `follow_logs()` and the log wait
handle the raw stream automatically (`Container` remembers its TTY-ness). No interactive
attach loop (exec's stdin path is the interactive tool).
