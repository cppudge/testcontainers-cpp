# Feature notes — what is implemented, and its known limits

Short reference notes on the implemented subsystems, distilled from the backlog as items were
completed. This file records what EXISTS, where it lives, and the caveats a user or maintainer
should know; the actionable leftovers live in [TODO.md](TODO.md).

## Transport & client

**Transports** (`src/docker/Transport.*`) — unix socket / Windows named pipe / TCP / TLS behind
one `ITransport`; `connect()` picks by scheme. TLS material selection is a per-connection
`TlsPlan` (`TlsConfig` helpers, unit-tested): a resolved host's docker-context materials are
used verbatim when present, else `DOCKER_CERT_PATH` / the `docker.cert.path` key (falling back
to `~/.docker` when TLS verify is on). Mutual TLS is verified end to end in CI against a real
`--tlsverify` daemon (the `tls-e2e` docker:dind job, which since 2026-07-11 also runs a
docker-context leg with no `DOCKER_*` connection env; fixed 2026-07-10 — the SSL context used
to be configured after the stream was created, so the client certificate was never presented
and server verification silently stayed fail-open; the ordering is pinned by two in-process
TlsTransportTest cases). The TLS transport is a build option (CMake `TC_TLS` / conan `tls`,
default ON) — the library's only direct OpenSSL use; with it off, `connect()` for an
`https://` host throws a `DockerError` naming the option, and the `TlsConfig` helpers
stay available. Host spelling: `https://host:port` works directly, and since 2026-07-11
`resolve()` also upgrades the docker-CLI spelling — `tcp://host:port` + `DOCKER_TLS_VERIFY`
(or `docker.tls.verify`) — to TLS, moving the default port 2375 → 2376; `parse()` never
upgrades an explicit URL.

**I/O deadlines** (`docker::TransportTimeouts`) — `connect` budgets the whole establishment
(resolve + connect + TLS handshake, default 10s); `io` deadlines each read/write (default 60s,
`nullopt` disables). Expiry throws `TransportTimeoutError` and the connection is discarded.
Long-poll endpoints widen the io deadline (`stop` past its grace period, `build` to 10 min);
the streaming call sites (`follow_logs`, exec attach) disable it after the response header —
except their deadline-bounded overloads, which re-arm it per read from the remaining budget.
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

**API version pin** (2026-07-05) — every typed `DockerClient` method pins its path to
`/v1.NN`, negotiated once per client instance the way the docker CLI does it: one unversioned
`GET /_ping`, then `min(kClientApiVersion = 1.44, daemon's Api-Version header)`, compared
numerically ("1.9" < "1.44"). 1.44 covers everything the library uses (newest need: the
`all` filter on volume prune, 1.42 — see the kClientApiVersion comment) and is the
negotiation floor of daemons that dropped the old
versions; an old daemon wins with its own version. Copies inherit the negotiated version (the
drop-time DELETE of a `Container` handle re-uses it — no second ping); a daemon that reveals no
parsable version falls back to unversioned paths (its default version, the old behavior). The
raw `request()` escape hatch stays deliberately unversioned. Residual: negotiation is lazy
per-instance state — same "one instance, one thread" rule as sessions.

**Docker host resolution** (`DockerHost::resolve`, `src/docker/HostResolve.hpp`) — the
testcontainers order, first hit wins: `DOCKER_HOST` → `docker.host` in
`~/.testcontainers.properties` → active docker context (`DOCKER_CONTEXT` / `currentContext`) →
platform default (rootless socket fallbacks on Linux; named pipe on Windows). Steps 2–4 never
throw on a malformed file. Since 2026-07-11 the context's TLS store
(`~/.docker/contexts/tls/<sha256>/docker/{ca,cert,key}.pem`) is consumed: its files attach to
the resolved host (`DockerHost::tls_materials()`), a `tcp://`-spelled endpoint with materials
dials TLS (CLI parity), and `Endpoints.docker.SkipTLSVerify` turns server verification off
while still presenting the client pair (verification is also gated on the store holding a
`ca.pem` — without a trust anchor the client pair is presented unverified). In steps 1–2 a
`tcp://` host upgrades to TLS under
`DOCKER_TLS_VERIFY` / `docker.tls.verify` (env certs apply as before). `parse()` never
upgrades; `ssh://` context endpoints remain unsupported.

**Configuration switches** (`src/Config.*`, 2026-07-11) — library switches read an env var
first, then a key of `~/.testcontainers.properties` (under HOME, else USERPROFILE; the file is
read ONCE per process). Keys: `docker.host`, `docker.tls.verify`, `docker.cert.path`,
`hub.image.name.prefix`, `host.override`, `ryuk.disabled`, `ryuk.container.image`,
`sshd.container.image`, `socat.container.image`, `compose.container.image`,
`testcontainers.reuse.enable` (env names = `TESTCONTAINERS_` + the key upper-cased with dots as
underscores, with two exceptions: the `docker.*` trio keeps its standard `DOCKER_*` names, and
a key already starting `testcontainers.` is not doubled — `testcontainers.reuse.enable` ↔
`TESTCONTAINERS_REUSE_ENABLE`, exactly like java's mapping).

**Host override** (2026-07-11, `detail::resolved_host_address`) — the address handed out for
reaching published ports: `TESTCONTAINERS_HOST_OVERRIDE` / `host.override` when set (any
scheme); else the daemon hostname for tcp/https; else "localhost" for a local socket/pipe —
unless the test process itself runs inside a Linux container (`/.dockerenv` exists, java's
`IN_A_CONTAINER` check), where "localhost" would be the container itself: the default
gateway parsed from `/proc/net/route` (the docker bridge address — java shells out to
`ip route` for the same answer) is returned instead. Consumed by `Container::host()`, the
HTTP/port wait probes, `DockerComposeContainer::get_service_host` (and its exposed-service
TCP probe), and the Ryuk registration endpoint — the transport's own Host header stays on
`DockerHost::http_host()`. This is the DinD/Testcontainers-Cloud enabler: with the socket
mounted into a CI container, set nothing (the gateway kicks in); with a remote agent, set
the override. Podman's `/run/.containerenv` is not probed (java parity).

**ConnectionString** (2026-07-11) — a small public builder assembling
`scheme://[user[:password]@]host[:port][/database][?k=v&…]` with per-component
percent-encoding (an IPv6 host is bracketed; the database is a single encoded path segment;
parameters keep insertion order). The DSN building block for the upcoming Tier-4 module
getters; deliberately NOT a URL parser — render-only.
A SET (non-empty) env var decides even when it decides "off" — an explicit env `false`
overrides a file-enabled switch (Java's `getEnvVarOrProperty` parity). Because the file is
shared with testcontainers-java, parsing mirrors it where the two could diverge: `#`/`!`
comment lines, last duplicate key wins, and boolean values follow `Boolean.parseBoolean`
(case-insensitive `true`; `1` is false) — except `docker.tls.verify`, where docker-java also
accepts `1`. Deliberate simplifications: `=` is the only separator; no escapes or line
continuations.

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
`spec.exposed_ports` vs `publish_all_ports`). Modules (Postgres/Redis/…, landed 2026-07-12)
are exactly that composition over `GenericImage` — see "Ecosystem modules" below.

**Lifecycle hooks + startup retry** — `with_created/starting/started/stopping_hook`
(`LifecycleHook = std::function<void(DockerClient&, const std::string& id)>`) and
`with_startup_attempts(n)` (the whole create→start→wait retried, each failed partial removed;
the reuse-adopt path is not retried; no inter-attempt backoff). A throwing created/starting/
started hook aborts start() and cleans up; stopping fires once on teardown — never for a
persistent (reuse) handle — and its exceptions are swallowed. `Container::stop(timeout_secs)`
(2026-07-12) forwards the daemon grace period (unset = the container's create-time
StopTimeout, default 10s; 0 = kill now; negative = wait indefinitely), and
`Container::start()` restarts a stopped handle through the OWNED client (so a
custom-daemon container restarts against its own daemon) — a plain daemon start: waits and
hooks do not re-run, the once-fired stopping hooks stay fired, and ephemeral published
ports are re-bound (re-resolve via `get_host_port()`; a module's Started* getters keep
their start()-time values).

**Reaper (Ryuk)** — containers, networks, named volumes, and built images carry the session
label; compose stacks are covered by an extra per-project filter registered over the same
control connection (each line ACKed). Pinned to `testcontainers/ryuk:0.11.0` (overridable via
`TESTCONTAINERS_RYUK_CONTAINER_IMAGE` / `ryuk.container.image`), skipped on the Windows engine
and via `TESTCONTAINERS_RYUK_DISABLED` / `ryuk.disabled`. Per-daemon: reapers are keyed by endpoint URL, so
`run(client, request)` against a second daemon boots a second Ryuk there and every daemon's
resources are crash-swept (two URL spellings of one daemon — e.g. `tcp://localhost` vs
`tcp://127.0.0.1` — count as two endpoints and get two harmless reapers). A daemon where Ryuk
CANNOT start now fails that run loudly (the fail-loud reaper contract; disable the reaper to
opt out) — it is no longer silently unwatched. Compose project
filters always go to the ENVIRONMENT daemon's reaper, where compose runs.

**Reuse** (`with_reuse`) — adopts an already-running container matching a stable FNV-1a
reuse-hash label (`org.testcontainers.reuse.hash`, over the create body + copy-to
descriptors: target, mode, bytes verbatim for byte sources, path + per-file size+mtime for
host sources — an in-place fixture edit changes the hash without content re-reads; the dir
walk mirrors the tar walk and is sorted). Gated on `TESTCONTAINERS_REUSE_ENABLE` /
`testcontainers.reuse.enable`, degrading to a normal container otherwise. The canonical
changed when freshness metadata was added (2026-07-10): reuse containers created by earlier
versions WITH copy-to sources hash differently and are not adopted — prune them (copy-free
reuse configs hash identically and are still adopted). Reused containers are never
auto-removed or reaped:
prune externally (label sweep on the reuse-hash label). `Container::keep()` flips a normal
handle into the same persistent state at runtime (no removal, no stopping hooks on drop;
`keep(false)` re-arms removal so a debug flag forwards in one call);
unlike reuse containers it KEEPS the session label, so on Linux engines Ryuk still reaps it
once the test process exits — disable the reaper (or use reuse) for a container that must
outlive the process. On a Windows-containers engine no reaper runs at all: a kept container
stays until removed manually.

**Wait strategies** — log message / fixed duration / exit(+code) / healthcheck / HTTP probe /
listening port / successful command, run in order under one shared startup timeout; the
inspect-based polls run inside a `DockerClient::Session`. The alternative TYPES live in
`wait_for::` next to the factories (2026-07-05, pre-0.1.0 rename): the former
`testcontainers::wait` namespace was ambiguous against POSIX `::wait(2)` under
`using namespace testcontainers;` on macOS. `wait_for::listening_port` probes the published
HOST port only (no in-container listen check), so a port published before the process binds
could read ready early. HTTP/TCP probes are deadline-bounded per probe (min(time left, 5s),
which absorbs Windows' ~2s refused-SYN retry on the dead `::1` half of "localhost").

**Command wait** (2026-07-11, `Wait.forSuccessfulCommand` analog) —
`wait_for::successful_command({argv...})` / `successful_shell_command("script")` (the latter
wraps `/bin/sh -c`) polls a deadline-bounded exec until an attempt exits 0. Non-zero exits AND
daemon-side errors (409 while the container is starting/restarting) are "not ready yet"; a 404
propagates (the container is gone — retrying cannot help); transport timeouts follow the log
wait's rule (at the wait deadline they ARE the readiness timeout, earlier they propagate). The
probe sends no stdin and no TTY, so it runs on every transport (TLS and named pipe included)
and its exec POSTs deliberately bypass the polling session (only GETs may ride it); the timeout
error reports the last COMPLETED attempt (exit code + a 512-byte output snippet) because the
final attempt is routinely cut off by the deadline and would otherwise mask it. An attempt
still running at the deadline is abandoned client-side but KEEPS RUNNING in the container
(Docker has no exec-kill API) — keep probes short-lived.

**Log wait streams** (2026-07-05) — the log-message wait rides ONE deadline-bounded follow
stream (`follow_logs(..., deadline)` → `FollowEnd`, re-arming the transport io deadline per
read) instead of re-fetching the full `tail=all` snapshot every 200ms; occurrences are counted
incrementally per stream (`detail::OccurrenceCounter`, chunk-boundary-safe, memory bounded by
the unmatched tail). If the stream ends (container stopped) it re-follows after ~200ms — a
restart-policy container can still produce the message — and an already-expired budget gets one
final snapshot check, so `timeout=0` with the message already logged still succeeds.

## Container features

**Port / inspect getters** — `get_host_port` (IPv4-preferred), `get_host_port_ipv4/_ipv6`,
`first_mapped_port()` (the first DECLARED port for handles from `start()`; lowest published
otherwise), `inspect()` / `inspect_raw()`; the static `Container::inspect(id)` is the same
lookup for an arbitrary container without a handle (read-only — unlike `adopt`, no ownership).
Uncached: every getter re-inspects the container.

**Logs** — `logs()` (snapshot) / `follow_logs()` (incremental streaming; BLOCKING with
cooperative stop via the consumer — run it on your own thread). TTY containers
(`with_tty`) use a raw/unframed decode path, selected automatically by `Container`; a
pseudo-TTY rewrites `\n` → `\r\n`, so match substrings, not exact lines.

**Exec** — `ExecOptions` (env / working_dir / user / privileged / tty / stdin_data / detach)
plus a streaming overload (`exec(cmd, opts, consumer)`) and its deadline-bounded variant
(`exec(cmd, opts, consumer, deadline)` → `ExecStreamResult`): everything after the start
response's header — the stdin writes included — is bounded by the absolute deadline (the
named-pipe half-close flush below is the one step no deadline can bound), and the result says
WHY delivery ended (`FollowEnd`) plus the exit code — present only when the command had
finished, because closing the attach stream does not kill it (a command cut off by the
deadline keeps running in the container; nothing in the Engine API kills an exec). An expiry
while connecting / creating / starting surfaces as `TransportTimeoutError` instead —
`DeadlineExpired` is reported once output could flow. `detach` is fire-and-forget
(`docker exec -d`): create + start as two plain round-trips (nothing attached, no
upgrade/hijack, no exec-inspect), the result keeps its defaults (empty output, exit_code 0 —
the command is still running, and a command that fails inside the container surfaces no
error), and detach+stdin / detach+consumer throw up front. Otherwise EVERY exec start
requests a connection
upgrade (`Upgrade: tcp` → HTTP 101, the exec stream then arrives raw, not as an HTTP body) —
docker-CLI parity, needed twice over: Docker Desktop's named-pipe proxy drops client bytes
sent after the POST on a non-upgraded connection (stdin would be lost), and some daemons never
terminate a non-upgraded exec-start response at all (observed on a Windows-containers 29.1.5:
output arrived, the EOF never did — CI hang pinned by a procdump stack). A daemon that ignores
the upgrade answers 200 and the HTTP-body read handles it (stdin then goes in sequentially
after the header — the pre-2026-07-11 behavior). On the upgraded stream, stdin (fed only
AFTER the response header) is INTERLEAVED with the output read — one full-duplex pump on the
transport's event loop — so a command echoing a multi-megabyte stdin back cannot backpressure
the write into a timeout; while stdin is still going out, the io deadline guards against a
peer consuming NEITHER direction, and once it is out, reads wait as long as the command runs.
All four overloads run ONE implementation — the buffered `exec(cmd[, opts])` is the streaming
path with an accumulating consumer (2026-07-11) — with one shared end-of-stream contract: any
read-side end (eof, a peer-closed pipe, a reset — dockerd resets the hijacked connection when
an exec exits with unconsumed stdin) is the peer finishing, the output received so far is
kept, and the exec inspect settles the exit code; only a wedged input phase (`timed_out`)
throws. Stdin EOF:
TCP / unix sockets half-close via `shutdown(send)`; the Windows named pipe mirrors go-winio's
`CloseWrite` (flush, then a zero-length message — message-mode pipes only, which is what every
real daemon serves; inside the pump the flush also pauses output reads while it blocks); TLS
cannot half-close, so exec-with-stdin throws up front there (Go's `tls.Conn` cannot either —
the docker CLI hangs where we throw). TTY sizing: `ExecOptions::console_size` sets a tty
exec's initial rows x columns at create (`ConsoleSize`, API 1.42+ — older daemons silently
ignore it), `ExecOptions::on_started` hands out the exec id at the first resize-valid moment
(started, nothing moved yet), and `resize_exec` / `Container::resize_tty` drive
`POST /exec/{id}/resize` / `POST /containers/{id}/resize` (SIGWINCH inside).

**Copy to / from container** — `CopyToContainer` (host file, in-memory bytes, or a recursive
host directory; `with_mode` applies to file entries) is PUT as a pax(restricted) tar — plain
USTAR until a field doesn't fit, so long paths and >8 GiB files work — STREAMED as a chunked
upload since 2026-07-10: host files are read in 64 KiB blocks as the chunks go out, nothing is
buffered whole, and a mid-upload daemon rejection surfaces as the daemon's status where the
transport delivers it (best-effort on named pipes). The batched
`copy_to_container(id, vector)` sends every source in ONE archive/PUT (the runner ships a
request's whole copy set that way; shared directory chains deduplicated, file collisions
last-wins). Targets may be drive-rooted (`C:\dir\x` ⇒ `/dir/x`). A single-file source needs a
pre-existing parent directory; a directory source ships its own target chain (dirs 0755, empty
dirs preserved, dir symlinks not descended). Copy-from: single regular files via `read_file` /
`copy_file_from` (buffered); whole trees via `copy_from_container_to(id, path, dest_dir)` —
a streaming download + untar (wire to disk, tar-slip protected incl. the Windows
interior-drive-letter escape, symlinks skipped by policy) — or the raw-bytes sink overload
`copy_from_container(id, path, sink)`. `container_path_stat(id, path)` HEADs the archive
endpoint for a cheap existence/size probe (decoded `X-Docker-Container-Path-Stat`).
`set_max_response_body(bytes)` caps what the BUFFERED paths will hold (a runaway body becomes
a DockerError instead of unbounded allocation); streaming paths are unaffected.

**Build from Dockerfile** (`GenericBuildableImage`) — Dockerfile from a host path or an inline
string; context from host files/dirs and in-memory data; `build()` returns a runnable
`GenericImage`. Since 2026-07-10 the context STREAMS: host files are descriptors read in
blocks while the chunked `POST /build` upload runs (`build_image` gained a `BodyProducer`
overload; the string overload wraps it — each block goes out under the base io deadline,
and the 10-minute silent-step widening starts only after the response header), and a
directory source honors a `.dockerignore` at
its root with docker-build (moby/patternmatcher) semantics — component globs, `**`, negation
last-wins, parent-dir exclusion, `^` in classes literal — while a root-mapped source's
`.dockerignore`/`Dockerfile` always ship (an explicit `with_dockerfile*` beats a walked-up
host Dockerfile). Build output streams live to
`with_build_log_consumer` (decoded line by line as the daemon emits it), and a failed build's
DockerError carries the tail of the step output — the failing RUN's own stdout/stderr — even
without a consumer. `GenericImage::exists(name, tag)` (constructor-style arguments; backed by
`DockerClient::image_exists`, which also takes full references/digests) is the local-presence
probe for skip-if-built flows; presence
says nothing about freshness, so derive the tag from a hash of the build inputs when they can
change. `GenericImage::inspect(name, tag)` / `img.inspect()` (backed by
`DockerClient::inspect_image[_raw]`, which also takes full references/digests) return a typed
`ImageInspect` — id, repo tags/digests, created, os/arch, size, and the image config (labels,
env, cmd, entrypoint, exposed ports, workdir, user); the instance form re-joins
`image()`/`tag()` digest-aware, no substitutor. Digest-pinned references
(`name@sha256:...`) work end to end (2026-07-12): `from_reference`/`with_image` split them
at the '@' (the digest becomes the tag slot), and every re-join — the create-spec
reference, the pull query (`?tag=` accepts a digest), the utility-image overrides — goes
through the shared digest-aware `join_image` ('@', never ':'). Built images are session-scoped: `build()` ships the managed-by /
session labels via `?labels=` (merged with the Dockerfile's own LABELs; on a duplicate key the
query label wins, `docker build --label` parity) and boots the reaper, so Ryuk removes the
image shortly after the process exits — base images and pulled layers are untouched. With the
reaper disabled the session label is omitted and the image persists; there is no per-image
keep switch (`TESTCONTAINERS_RYUK_DISABLED` is process-wide). The raw
`DockerClient::build_image` adds no labels of its own — `BuildOptions::labels` is
caller-controlled. `forcerm=1` is always sent — without it the legacy builder keeps a failed
step's intermediate container, which carries no labels and leaks past Ryuk.

**HostConfig** — typed setters for memory / shm_size / ulimits / cap_add / cap_drop /
extra_hosts / cpu limit (`with_cpu_limit(double)` → NanoCpus, `docker run --cpus` parity;
CpuShares/CpuQuota/CpuPeriod stay untyped — NanoCpus subsumes them) / cpuset / pids limit /
restart policy (`RestartPolicy` factories; the daemon rejects one combined with auto-remove,
a nonzero retry count is only valid with on-failure, and Ryuk still force-removes at process
exit — `always` does not outlive the session) / dns servers+search+options / sysctls
(namespaced only; Linux) / devices (`Device`; Linux path semantics — Windows devices use
class GUIDs); everything else through `with_create_body_patch` (an RFC-7386 deep-merge
applied AFTER the typed fields, so it overrides them; nest HostConfig fields under
`"HostConfig"`). `ContainerInspect::host_config` echoes the typed knobs back (the daemon's
zero state when unset: 0 / "" / empty; PidsLimit null and 0 both mean "no limit"), so tests
assert a landed limit without parsing `inspect_raw()`. Unset fields emit nothing into the
create body, so reuse hashes of configs predating these setters are unaffected.

**Networks** — `Network` RAII handle + builder (driver / internal / attachable / IPv6 / IPAM
pools / driver options / labels). IPAM: `with_subnet`/`with_gateway` configure a leading
shorthand pool; repeatable `with_ipam_pool` (2026-07-11) adds full pools — subnet, IPRange,
gateway, auxiliary addresses — after it (one IPv4 + one IPv6 pool is the Linux bridge
driver's maximum; multiple IPv4 pools need a driver that supports them). The IPAM driver
and its options stay unmodeled (the generic `request()` escape hatch covers them if ever
needed).
`GenericImage::with_network` takes the
handle directly or a name string (the handle overload records `name()` — ownership stays with
the handle); `with_network_alias` (per-network DNS
aliases); `with_static_ipv4` (a fixed endpoint address — needs a user-defined network whose
subnet contains it); `Network::connect` attaches a running container. Inspect: `net.inspect()`
/ the static `Network::inspect(name_or_id)` return a typed `NetworkInspect` (driver, scope,
internal/attachable/IPv6 flags, IPAM pools incl. IPRange and name-sorted auxiliary
addresses, options, labels, and the attached containers'
endpoints — addresses in CIDR form), `net.inspect_raw()` the full JSON (both over
`DockerClient::inspect_network[_raw]`). `net.keep()` releases removal ownership —
`Container::keep` semantics, including the Ryuk caveat (the network stays session-labeled, so
the reaper still removes it after the process exits) and the `keep(false)` re-arm.
`Builder::with_reuse` (2026-07-11) is find-or-create with `GenericImage::with_reuse`
semantics: gated on the same global enable, matched by a config-hash label plus the exact
`with_name` (required), adopted/created networks are persistent and NOT session-reaped
(external cleanup = the reuse-hash label sweep), and a same-name network with a DIFFERENT
config makes `create()` throw rather than add an ambiguous duplicate (Docker does not
enforce unique network names). `DockerClient::list_networks(filters)` lists networks
(label/name filters; the daemon matches names by substring). No process-wide dedup of
non-reuse networks — every plain `create()` makes a brand-new network.

**Host access (`with_exposed_host_port`)** — services listening on the test host become
reachable from containers at `host.testcontainers.internal:<port>` through the standard
Testcontainers `testcontainers/sshd` sidecar: one per daemon (started on the daemon's first
use, session-labeled for Ryuk, removed on clean exit), each with one SSH session (libssh2)
carrying a remote forward per exposed port; connections arriving at the sidecar are pumped back to
`127.0.0.1:<port>` in the test process, so it works wherever the daemon runs (Desktop VM,
remote engine). Supported on the default bridge and user-defined networks (the sidecar joins a
user network on demand; `Network` teardown detaches it again); requires a Linux-containers
daemon; network modes "host" / "none" / "container:..." are rejected. The whole feature is a
build option (CMake `TC_HOST_PORT_FORWARDING` / conan `host_port_forwarding`, default ON) — it
is what pulls in libssh2 (and, together with `TC_TLS`, OpenSSL: libssh2's crypto backend);
with it off, `start()` of a request carrying host-access ports throws a `DockerError` naming
the option. Residuals: remote
forwards are never cancelled once added; the sidecar's root password travels in the create
body's env (readable via inspect — by someone who already has daemon access; Java parity).

**Named volumes** — `Volume` RAII handle + builder, session-labeled for Ryuk;
`populate(sources)` seeds data through a throwaway helper container (`alpine:3.20` by default,
started before the archive PUT — not every daemon materializes writes on a created-only
container's mountpoint). On Windows daemons (2026-07-11) populate stages into the CREATED
(not yet started) helper's layer (Hyper-V isolation rejects archive writes to a running
container; extraction bypasses mounts either way) and xcopies onto the volume from inside as
ContainerAdministrator; default helper nanoserver:ltsc2022 — pass a build-matched image on
process-isolation daemons. `DockerClient::list_volumes(filters)` lists volumes (label/name
filters; names match by substring) and `prune_volumes(filters)` batch-removes unused ones,
returning the daemon's deleted-names + reclaimed-bytes report (2026-07-11) — API 1.42+
daemons prune only ANONYMOUS unused volumes unless the `{"all","true"}` filter is passed.
The RAII drop fails (409) while a container still mounts the volume — tear down in
reverse-declaration order.

**Image pull policy + name substitution** — `ImagePullPolicy::Default` (lazy: pull on a create
404) / `Always` (pull before every create) / age-based
(`with_image_pull_policy(std::chrono::seconds)`, 2026-07-11, java `PullPolicy.ageBased`
parity): under `Default` with an age budget the run inspects the local image and re-pulls
when its `Created` timestamp is older than the budget or unreadable (Go's zero time and
garbage both count as stale — the safe default is a refresh); a missing image stays on
create's lazy path. The shared caveat: `Created` is the image's BUILD time, not the pull
time, so an old image re-pulls every start even when the registry holds the same bytes
(docker records no pull time). The RFC 3339 parsing is a pure helper
(`docker::parse_rfc3339`, integer math, no timegm). Substitution via
`TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX` (or the `hub.image.name.prefix` properties key) or a
custom `with_image_name_substitutor` (replaces the default, `GenericImage`-scoped). Since
2026-07-11 the hub prefix reaches EVERY internal utility image — ryuk, the socat ambassador,
the containerised compose cli, the volume-populate helpers, plus the sshd host-access sidecar
(which already had it via `GenericImage`) — each of which also has a configured default
(env / properties): `ryuk.container.image`,
`sshd.container.image`, `socat.container.image`, `compose.container.image` (helpers take a
per-call `helper_image` parameter instead). The prefix only touches Docker-Hub references, so
a registry-qualified mirror override passes through untouched. Still NOT substituted: images
built by `GenericBuildableImage` (the daemon resolves `FROM` lines), services in compose YAML
(the file is the user's), and raw `DockerClient` calls (deliberately verbatim).

**Pull retry** (2026-07-10) — `pull_image` retries an HTTP 5xx reply to `POST /images/create`
(the daemon relaying transient registry trouble, e.g. its auth-token endpoint answering
500/502) up to 3 total tries with a 1s-then-2s backoff; tune or disable via
`DockerClient::set_pull_retry`. Deliberately narrow: 4xx and in-stream pull errors (how most
daemons report a nonexistent image) fail on the first try, and transport timeouts are never
retried. Caveat: some daemons relay a bad image name as a header 500 too — those pay the
backoff (~3s) before failing.

**Registry auth** — explicit `with_registry_auth` or auto-resolved from the Docker config
(`DOCKER_AUTH_CONFIG` → `$DOCKER_CONFIG/config.json` → `~/.docker/config.json`), including
credential helpers (`credsStore` / `credHelpers`, shelling out to
`docker-credential-<helper> get`; plaintext `auths` take precedence). Helper outcomes —
including "no credentials", the answer for every anonymous pull under Docker Desktop's
`credsStore` — are cached process-wide per (helper, registry) for 5 minutes (2026-07-10);
the config file itself is still re-read per pull.

## Ecosystem modules

**The module layer** (2026-07-12, `testcontainers::modules` — its own library target and
Conan component) — prebuilt wrappers over `GenericImage`, one per technology: a copyable
config builder (`modules::RedisImage`) whose `start()` returns a move-only started
handle (`modules::RedisContainer`) owning the core `Container` and carrying the credentials,
host, and mapped port by value — resolved once at `start()`, so the connection getters are
pure (no daemon round-trips; a container restarted by hand gets fresh ephemeral ports — drop
to `container()` to re-resolve). The naming deliberately mirrors the core split: `XxxImage`
is the reusable recipe, `XxxContainer` the running instance, exactly like `GenericImage` →
`Container` (renamed pre-0.2.0 from the node-style `XxxContainer`/`StartedXxx` pair, which
gave the word "container" opposite meanings on the two layers). No client drivers — the handles
hand out host/port and DSN strings (assembled with `ConnectionString`); the integration
proof execs the in-container CLI instead. Two escape hatches set the pattern for every
module: `with_customizer(fn)` queues a callback over the underlying `GenericImage`, run at
render time AFTER the module's own rendering (what it sets wins — the same precedence idea
as `with_create_body_patch` over typed fields), and `to_generic()` renders the full config
into a plain `GenericImage` for when a raw core `Container` is wanted. Each module documents the
small surface it owns (cmd/env keys); everything else passes through untouched. Aligned in
the pre-0.2.0 audit pass (2026-07-12): every module surfaces the same curated pass-through
set — env / label / network (name or `Network`) / network-alias / reuse / startup-timeout /
startup-attempts; `with_startup_timeout` is a PER-PHASE budget (one phase for most modules;
Kafka and MongoDB run a second, hook-driven phase with a fresh allowance of the same size);
command-line arguments use one spelling pair wherever they apply (`with_command_arg` single
/ `with_command_args` batch — Redis and the MySQL family); and a string argument to
`connection_string(...)` always means the DATABASE (MongoDB) — scheme variants get their
own name (PostgreSQL's `connection_string_with_scheme`). `testcontainers/modules.hpp` is
the modules umbrella header (compile-checked by the Redis unit TU). Opt-in by
include + link: headers under `include/testcontainers/modules/`, target
`testcontainers::modules` — the one spelling that means the same thing on both consumption
paths. Around it the paths differ: the plain-CMake install exports
`testcontainers::testcontainers` (core ONLY) + `testcontainers::modules` and has no
`::core` alias, while under Conan the root `testcontainers::testcontainers` AGGREGATES
core+modules (components `testcontainers::core` / `testcontainers::modules` select a
layer). Portable consumers therefore link `testcontainers::modules` explicitly for the
module layer — a Conan link line leaning on the root's aggregation breaks against a
`cmake --install` tree. Module integration tests live in their own `tc_module_tests`
executable (ctest label `modules`, engine-guarded like the core suite): CI runs them on the
primary Linux job, and on the Windows job only to prove the guards self-skip; consciously
NOT in the sanitizer/TLS jobs — the layer is thin composition over already-sanitized core
paths. One core addition rode along: `GenericImage::with_image("name[:tag]")` re-points a
configured builder at another image (same parsing as `from_reference`).

**Redis module** (2026-07-12, `modules::RedisImage` → `modules::RedisContainer`) — pinned
`redis:7.2`, port 6379 exposed, readiness = in-container `redis-cli ping` via the command
wait (a log wait races the listener; a raw TCP probe false-positives through Docker
Desktop's host proxy). `with_password` renders `{"redis-server","--requirepass",pw}` AND
sets container-level `REDISCLI_AUTH`, so the unchanged probe — and any `redis-cli` the user
execs — authenticates automatically (honored by redis-cli ≥ 4.0.10; the pin is far past
it). `with_command_arg[s]` append server arguments after `--requirepass`; argv[0] stays
`redis-server` so the official entrypoint's protected-mode handling keeps applying. The
module owns the container command (iff a password or args are set) and the `REDISCLI_AUTH`
env key — nothing else. The env key is guarded, not just documented: `with_env` carrying
REDISCLI_AUTH alongside a password throws at render — the key is read by EXEC'D redis-cli,
where the FIRST duplicate of a key wins, so the append-last ordering that protects the DB
modules' bash-read credential keys could not make the module's entry win here.
`connection_string(db = 0)` renders
`redis://[:pass@]host:port[/db]`; database selection is client-side in Redis, so there is
no server-side database setter. Known limits: a config FILE must be the first server
argument, so a config file combined with `with_password` is unsupported (drop to a
customizer's `with_cmd`); redis-stack / Sentinel / cluster are out of scope (different
image families / multi-container topologies).

**PostgreSQL module** (2026-07-12, `modules::PostgreSQLImage` →
`modules::PostgreSQLContainer`) — pinned `postgres:16-alpine` (C locale; the header points
collation-sensitive tests at the Debian image / POSTGRES_INITDB_ARGS), credentials
test/test/test, readiness = exec `pg_isready -h 127.0.0.1 -p 5432 -U <user> -d <db>`. The
TCP-forced probe is the load-bearing choice: the image's first boot runs a TEMPORARY
unix-socket-only server (initdb + /docker-entrypoint-initdb.d), shuts it down, then starts
the real TCP server — a socket probe or the "ready to accept connections" log line
(printed by BOTH servers) reads ready inside that window, the TCP probe cannot, and TCP
readiness additionally proves every init script finished. The credential-env trio is
module-owned: appended last so it wins over raw `with_env` duplicates, and an empty
password fails fast at render time unless `POSTGRES_HOST_AUTH_METHOD=trust` was set (the
image's own failure mode is a log line plus the full wait timeout). `with_init_script`
(host file or in-memory) targets /docker-entrypoint-initdb.d with a zero-padded
registration-index prefix (registration order beats the entrypoint's C-collation name
order), whitelists the extensions the entrypoint executes (.sql, .sql.gz, .sql.xz,
.sql.zst, .sh — anything else throws instead of being silently skipped), ships `.sh`
executable (a non-executable .sh is *sourced* into the entrypoint's shell, where a stray
`exit` kills the boot), and is not re-run on a reuse-adopt (data persistence is the point;
editing a script changes the reuse hash). `with_config_option` renders `postgres -c k=v`;
`with_wait` REPLACES the default probe (a customizer-added wait runs in addition).
`PostgreSQLContainer` adds `conninfo()` (libpq keyword/value with its quoting rules; the
password keyword omitted when empty) beside the URI `connection_string()` /
`connection_string_with_scheme()`, and
`exec_sql()` through the in-container psql (`-X -tA`, local-socket trust — no password).
Curated pass-throughs (env / label / network / alias / reuse / timeout / attempts) forward
to the embedded builder; everything else rides `with_customizer`.

**MySQL + MariaDB modules** (2026-07-12, `modules::MySQLImage` / `MariaDBImage` →
`MySQLContainer` / `MariaDBContainer`) — two flat public classes over one shared core
(`src/modules/MySqlFamily.*`: the boot matrix, init-script staging, probes, rendering, URL —
everything that must not fork), keyed by a small flavor table (env-contract names, probe).
Pins: `mysql:8.4` / `mariadb:11`; both default to a 120s startup budget (a first boot
initializes the datadir). The root-password boot matrix guarantees every start carries a
root decision: a non-root user shares its password with root (a known superuser on every
path), `with_username("root")` (case-insensitive) switches to root-only provisioning and
deliberately emits no `*_USER` key (both images refuse user=root), root+empty password
emits the flavor's allow-empty key, and non-root+empty fails fast at render (the images'
own failure mode is an entrypoint error plus the full wait budget). Readiness: both images'
first boot runs a TEMPORARY socket-only bootstrap server (--skip-networking) that
provisions credentials and runs /docker-entrypoint-initdb.d before the real TCP server
starts — the log line prints for BOTH acts, so the probes force TCP: MySQL exec
`mysqladmin ping -h127.0.0.1 -u<user> [-p<pw>]` (exits 0 even on access-denied — it
measures liveness, immune to credential edge cases), MariaDB the image's own
`healthcheck.sh --connect --innodb_initialized` (credential-free, and it sidesteps the
image's renamed client binaries — `mariadb`/`mariadb-admin`, not the deprecated mysql-*
names). 500ms poll (each attempt is a fresh exec connection; 200ms is churn against a boot
measured in tens of seconds). Init scripts and `.cnf` drop-ins reuse the PostgreSQL
staging rules (NNNN- registration-order prefix, extension whitelist, .sh 0755;
`/etc/mysql/conf.d` names must end in .cnf — the include glob skips others silently).
`with_command_arg[s]` values become the container cmd verbatim (the entrypoints forward
'-'-prefixed args to the server binary). Both `connection_string()`s emit the **mysql://**
scheme — MariaDB speaks the MySQL wire protocol and URL-parsing clients widely reject
"mariadb://"; `root_password()` documents the root≡user invariant at call sites. Known
limit (recipe in the `with_command_arg` header note, not module-fixable): MySQL 8.4
disables the `mysql_native_password` plugin by default, so pre-8.0-era client stacks need
`with_command_arg("--mysql-native-password=ON")` plus an ALTER USER init script.

**Kafka module** (2026-07-12, `modules::KafkaImage` → `modules::KafkaContainer`) — a
single-node KRaft broker (no ZooKeeper), pinned `apache/kafka:3.9.1` (official ASF image:
small, ships the CLI tools, the last 3.x line for maximum client-protocol compatibility).
Kafka cannot be "env + port + wait": the broker must ADVERTISE an address, the host-side
address contains the mapped port — which does not exist until after start — and listeners
are read once, at boot. The module runs the classic two-phase boot: the container starts
with a placeholder command (echo a sentinel, poll for `/tmp/testcontainers_start.sh`, exec
it), the request-level wait gates on the sentinel, and the STARTED HOOK does the rest —
resolves host + mapped port (via the same `Container::host()` path the getters use, so the
advertised host and the user's host can never diverge), writes the starter script
(exports KAFKA_ADVERTISED_LISTENERS with the real port, then execs the image's launch
script — apache path, confluent fallback), follows the logs (deadline-bounded, tail=all)
until "Kafka Server started", and pre-creates `with_topic` topics. A readiness timeout in
the hook throws StartupTimeoutError carrying the last log lines; a topic-creation failure
throws DockerError — both abort start() with cleanup and participate in startup retries.
Three listeners: PLAINTEXT 0.0.0.0:9092 (the only published one; host-side clients),
BROKER 0.0.0.0:9093 (peers on the docker network + in-container CLI; doubles as the
inter-broker listener), CONTROLLER 9094 (the node's own quorum — never advertised, KRaft
rejects that). THE trap the module encodes: in-container clients must bootstrap
`localhost:9093`, never `:9092` — Kafka's metadata reply carries the advertised address of
the listener the connection arrived on, and :9092's is the host-side address, unreachable
from inside. `bootstrap_servers()` returns bare `host:port` (librdkafka rejects a
PLAINTEXT:// scheme); `internal_bootstrap_servers()` returns `<first alias>:9093` (short
container id without an alias). The env set is COMPLETE (any user config makes the image
drop its baked-in defaults) and user `with_env` lands after the module's — user wins on
duplicates (broker tuning, unlike credential-mirroring DB modules; CLUSTER_ID belongs to
`with_cluster_id`, validated at render: 22 URL-safe base64 chars, fixed default for reuse
determinism). Readiness is log-based, not exec-based, on purpose: a command probe spawns a
JVM per poll and would break `apache/kafka-native` overrides. `with_topic` also renders a
reuse-visible label (the topic list otherwise lives only in the hook lambda, invisible to
the reuse hash). `with_startup_timeout` budgets EACH phase (worst case ≈ 2×). Out of
scope: SASL/TLS listeners, multi-broker quorums, Schema Registry et al. (separate
modules), confluent images (untested best-effort: the starter script's fallback covers
the boot only — `with_topic` still execs the apache CLI path).

**RabbitMQ module** (2026-07-12, `modules::RabbitMQImage` → `modules::RabbitMQContainer`)
— pinned `rabbitmq:3.13-management` (the management variant on purpose: its HTTP API is
the one broker-inspection surface a no-drivers test can hit, and the weight argument is a
myth — the tag differs from the plain image essentially by `enabled_plugins`, not a fat
layer). Ports 5672 (AMQP) + 15672 (management) published. Readiness is ORDERED and the
order is load-bearing (verified live during design): `wait_for::log("Server startup
complete")` FIRST, one `rabbitmq-diagnostics -q check_port_connectivity` exec second, 1s
poll — the image has no USER directive, so exec runs as root, and any Erlang CLI in the
first ~2s of boot creates a root-owned 0400 `.erlang.cookie` that the uid-999 server then
cannot read: the node dies unrecoverably inside that container. A log wait never execs;
by the time the diagnostics probe runs, the server long since wrote its own cookie.
`with_definitions`/`with_definitions_json` answer the second live-verified trap: under
`load_definitions` RabbitMQ skips ALL default provisioning (a definitions file with no
"users" entry leaves the broker with ZERO users, guest included — the env vars are ignored
wholesale). The module therefore imports a DIRECTORY (`/etc/rabbitmq/definitions.d`):
a synthesized `0010-testcontainers-seed.json` carrying the configured
user/password/vhost/permissions (plaintext password fields — the importer hashes on load),
the user's files as `05NN-` (call order; lexicographically after the seed, so a user file
declaring the same objects wins), plus a `load_definitions` conf drop-in in
`/etc/rabbitmq/conf.d`. So `with_definitions` composes with the credential setters instead
of silently disabling them. `with_plugin` enables plugins post-ready via
`rabbitmq-plugins enable` (additive — overwriting `enabled_plugins` at boot would drop the
image's own management/prometheus) and renders an order-normalized reuse-visibility label,
so a changed plugin set creates a fresh container instead of adopting one without the new
plugins. An empty password fails fast at render — verified live: the account gets created
but the broker's internal auth backend prohibits blank-password logins outright.
`amqp_url()` emits NO path for the default vhost "/"
(an absent path means the client's default vhost in every mainstream client, and unlike
the spec-equivalent `/%2F` it survives URI parsers that skip percent-decoding); any other
vhost renders percent-encoded as one segment. `with_username` REPLACES the built-in guest
account (image contract); remote guest works only by the official image's
`loopback_users.guest = false` grace — custom users sidestep that on hardened bases. Out
of scope: clustering, TLS/amqps, MQTT/STOMP typed getters, tc-java's ~30-method
per-object topology builder (definitions import is RabbitMQ's own bulk mechanism).

**MongoDB module** (2026-07-12, `modules::MongoDBImage` → `modules::MongoDBContainer`) —
pinned `mongo:7` (floor `mongo:5.0` — the module execs `mongosh`, argv form only; older
images ship just the removed legacy shell), and it ALWAYS runs as a single-node replica
set (`--replSet rs0 --bind_ip_all`): transactions and change streams are why the module
exists, a standalone rejects both, and the whole cost is a ~1–2s election on top of the
boot. There is deliberately no standalone mode and no auth surface — MongoDB requires a
cluster keyfile the moment auth meets a replica set, so MONGO_INITDB_ROOT_* is a
boot-breaker under --replSet (`with_env` rejects both root keys at render; the header
warns customizer users off them; initdb.d scripts are equally unsupported — they trigger
a temporary double-start whose log line would release the wait early). Boot choreography:
log wait "Waiting for connections" (exact 4.4+ casing; appears exactly once since nothing
triggers the initdb phase) → listening_port (proves the HOST side of the mapping) → a
started hook that execs `rs.initiate({_id, members: [{_id: 0, host:
'127.0.0.1:27017'}]})` (deterministic self-check member address; AlreadyInitialized
tolerated for pre-initiated volumes and reuse) and then polls
`db.hello().isWritablePrimary` in-shell on 100ms ticks under a fresh phase budget (the
configured startup timeout — the same per-phase contract as Kafka's two-phase boot),
chunked into execs of ~15s so the wall-clock phase deadline is re-evaluated between
chunks and a transiently failing mongosh retries (the buffered exec itself runs with the
transport deadline disabled — the hook's steady_clock deadline is the one authority) —
the PRIMARY wait cannot be a wait strategy because waits run before hooks, i.e. before
rs.initiate exists. THE DSN decision: `connection_string()` emits
`mongodb://host:port/<db>?directConnection=true` and NEVER `replicaSet=` —
direct mode pins single-server behavior in every spec-compliant driver instead of relying
on per-driver legacy defaults (the "works in tc-java, ServerSelectionTimeoutError in
PyMongo" classic), while a direct connection to a PRIMARY fully supports sessions,
transactions, and change streams; the database segment is always present, empty name
included (`/?directConnection=true`), because strict URI parsers reject options without
the slash. `mongosh(js)` on the Started handle seeds and asserts without a C++ driver.
Reuse-adopt skips the hook — correct: the RS config persists in the data directory. Out of
scope: auth/keyfile choreography, initdb.d scripts, multi-node sets, sharding.

**NATS module** (2026-07-12, `modules::NATSImage` → `modules::NATSContainer`, wave 2) —
pinned `nats:2.12` (the maintenance train; the plain FROM-scratch variant on purpose:
neither it nor `-alpine` ships a NATS CLI, so a shell buys the module nothing — there is no
redis-cli analog to exec). Ports 4222 (client) + 8222 (HTTP monitoring, always on via the
module's `-m 8222`); 6222 stays unpublished (a single node has no cluster peer). Readiness
is ORDERED: `wait_for::log("Server is ready")` (the server logs to stderr; the either-stream
wait survives an upstream logging change) then `wait_for::http("/healthz", 8222)` — the
end-to-end proof through the published port that a TCP wait can't give against Docker
Desktop's accept-anything host proxy; under `-js` the same endpoint also gates JetStream
health. The server reads NO env — the whole config is command-line flags, so the module
always owns cmd (an override drops the image's stock config file, whose entire content is
the default ports — monitoring is restated unconditionally, one code path on every image
variant, and the leading `-` keeps the alpine entrypoint routing args to nats-server).
Because Go's flag parsing keeps the LAST occurrence of a duplicate, append-last ordering
protects nothing here (the exact inverse of the DB modules' env rule) — render THROWS on
managed flags inside `with_command_args`: `--user`/`--pass` (with_username/with_password —
pair-or-nothing, half a pair throws), `-js`/`--jetstream` (with_jetstream),
`-m`/`--http_port` (a `-m 0` would starve the /healthz wait for the full budget),
`-p`/`--port`/`-a`/`--addr`/`--net` (the client listener the getters resolve), and
`-c`/`--config` (a config file re-sets any of the above invisibly); `-`/`--` and `=`-forms
all match. Everything else appends verbatim after the managed flags (server tuning — the
user wins); config-file setups take full command ownership via a customizer's `with_cmd`.
No hooks, so the reuse hash covers the entire config via Cmd — no module label needed; an
adopted server keeps its JetStream streams (stream data lives in the container layer
otherwise — documented on with_jetstream, with a volume-over---store_dir customizer recipe).
`url()` renders `nats://[user:pass@]host:port` (percent-encoded; the NATS_URL shape),
`monitoring_url()` plain `http://host:mport`. Integration proof is host-side raw TCP — the
CRLF text protocol is the driver (INFO greeting, PING→PONG, `Authorization Violation`
pre-CONNECT, `CONNECT {"user":...}` then PONG, `--name` echoed as INFO server_name) plus a
raw HTTP /healthz GET. NOTE: nats-server's command line carries only a small option set —
server tuning like max_payload is config-file-only (usage verified live; the module's flag
examples stick to real flags: --name, --auth, -DV). Out of scope: clustering/leaf nodes
(multi-container), `--auth` token as a typed setter (rides with_command_args; a URL form
would need `nats://token@`).

**Mosquitto module** (2026-07-12, `modules::MosquittoImage` → `modules::MosquittoContainer`,
wave 2) — pinned `eclipse-mosquitto:2.0` (2.0.x is the patch-receiving production line; the
floating `2` tag jumped to 2.1.2-alpine in 02/2026 — exactly what a version-stable pin
avoids; at the 2.1 bump re-verify the "running" line/stream and the new `-alpine` tag
suffix). Port 1883 only; websockets stay a documented config-opt-in recipe. THE trap the
module exists to encode away: since 2.0 a configless broker listens on the container's
loopback ONLY, so the mapped port connects to nothing — the module ships a managed
mosquitto.conf (`listener 1883` + `allow_anonymous true`, matching the image's own no-auth
quickstart file) as CopyToContainer::content to /mosquitto/config/mosquitto.conf,
overwriting the stock file pre-start. Config is file-only (the image reads no env → no
managed env keys, with_env is a pure pass-through). Config surface: with_config_option(k,v)
appends lines after the managed block in call order (listener-scoped options bind to the
1883 listener); with_config(path)/with_config_content(bytes) REPLACE the file entirely
(last call wins); combining options with a replacement THROWS at render — mosquitto config
is order- and listener-scoped, merging has no well-defined meaning (replace-entirely was
the chosen precedence). Replacement owners keep the contract: a listener 1883, logs on
stdout/stderr (the wait scans them), their own auth (allow_anonymous defaults to false once
a listener is defined). Readiness = wait_for::log("running") — the broker prints "mosquitto
version X running" AFTER listeners__start() (verified in the 2.0.x sources), so no
log/listener race and no exec probe to break under replaced auth configs; default log_dest
is stderr, the either-stream wait survives log_dest overrides. No hooks → no reuse label;
the managed config is generated as a pure function of the options (fixed comment header, no
timestamps) so identical configs stay reuse-hash-identical (byte copy-to sources hash
content verbatim; with_config(host_path) hashes path+size+mtime — the core's caveat).
Auth DEFERRED by research, not dodged: the official TLS-compiled broker requires HASHED
password_file entries (plaintext works only on no-TLS builds), client-side PBKDF2-SHA512
hashing would drag optional-OpenSSL into the modules layer, and in-container
mosquitto_passwd needs a started broker + SIGHUP + root-vs-uid-1883 ownership dance
(RabbitMQ's cookie trap class). Recipe today: with_config naming a password_file +
customizer with_copy_to of a pre-hashed file; typed with_password_file is the follow-up.
Integration proof: raw MQTT 3.1.1 CONNECT/CONNACK helper (tests/integration/MqttConnect.hpp,
RedisPing mold — return code 0 anonymous-accepted, 5 not-authorized for the replacement
test), retained-message pub/sub round trip through in-image mosquitto_pub/_sub (retained ⇒
no blocking-subscriber race; `-C 1 -W n` bounds every subscribe, timeout exits 27),
$SYS/broker/version as the pin-drift canary (argv exec — no shell to eat `$SYS`), and
retain_available=false as the observable negative proving appended options reach the real
broker (the QoS-0 publisher may exit 0 before noticing the disconnect — the subscriber
timeout is the reliable observable). Out of scope: typed auth (above), websockets listener,
bridges/plugins.

**ClickHouse module** (2026-07-13, `modules::ClickHouseImage` → `modules::ClickHouseContainer`,
wave 2) — pinned `clickhouse:26.3` (the Docker Official Image; 26.3 is the current LTS line,
supported to 2027-03; 24.x/25.3 are EOL — re-pin to the newest LTS ~every 6 months). Ports
8123 (HTTP) + 9000 (native protocol), both published: clickhouse-cpp speaks native via
discrete ClientOptions (no DSN parser — the handle leads with discrete getters and
`native_port()`), HTTP serves curl/JDBC/ODBC via `http_url()`. Credentials test/test/test
via the CLICKHOUSE_USER/PASSWORD/DB trio, DB env rule (appended LAST, module wins — bash
entrypoint applies the last duplicate). The credential decision is load-bearing, not just
house style: the image RESTRICTS a passwordless `default` user to the container's loopback
(host-side clients can never connect), and provisioning a custom user REMOVES the built-in
`default` — so empty password throws at render (throw-always; deliberately open servers
drop to a plain GenericImage), and the provisioned user is the only account. Readiness =
an ORDERED TRIPLE, each leg load-bearing and each earlier form falsified live during
implementation: whenever CLICKHOUSE_DB or initdb.d is set (every start of this module) the
entrypoint first runs a TEMPORARY server bound to 127.0.0.1 for provisioning + init
scripts — and NO network probe reliably tells it from the real server, because Docker
Desktop's port proxy reaches loopback-bound listeners through the published port (observed:
/ping answered 200 out of the init window, and the exec'd SELECT hit the temp server —
UNKNOWN_TABLE — while init scripts were still running; on a namespace-faithful Linux daemon
the published-port probe alone WOULD suffice, which is exactly why CI would never have
caught it). The disambiguator is the process tree, not the network: the entrypoint finishes
init by EXEC'ing the real server over itself (its own comment: "This replaces the shell
script with the server"), flipping /proc/1/comm "entrypoint.sh" → "clickhouse-serv". So:
(1) `successful_shell_command("grep -q clickhouse /proc/1/comm")` — real server is PID 1,
temp gone, every init script done (a failing script aborts the boot); (2)
`wait_for::http("/ping", 8123)` through the PUBLISHED port — end-to-end mapping proof, now
guaranteed real; (3) in-container `clickhouse-client SELECT 1` — the server opens 8123 a
beat before 9000 (an API exec is fast enough to hit that gap: NETWORK_ERROR observed right
after the first /ping 200; a hand-run docker exec never caught it — its process-spawn
latency was the sleep), and it proves the provisioned credentials. Single phase, house 60s
(cold boot ≈5–15s; no hooks). Init scripts reuse the shared ModuleDetail staging (NNNN-
registration prefix, .sh shipped 0755) but with a NARROWER whitelist — the clickhouse
entrypoint executes .sql/.sql.gz/.sh only (no xz/zst; ModuleDetail gained explicit-whitelist
overloads for exactly this). Two script contracts verified live on 26.3 (both differ from
postgres): the entrypoint's client runs with NO default database (CLICKHOUSE_DB is created
but not selected — scripts must qualify names or `USE <db>;`; multiquery is on), and a
FAILING script aborts the whole boot (container exits carrying the script's error — e.g.
sleepEachRow is capped at 3s per block, so a 6s single-block sleep kills the start). `with_config_file` ships .xml/.yaml/.yml drop-ins under their
own names into /etc/clickhouse-server/config.d (.yml merge verified live on 26.3:
max_connections via yml visible in system.server_settings). `with_wait` REPLACES the
default probe (postgres semantics); customizer waits ADD. `exec_sql` runs one statement via
in-container clickhouse-client (--user/--password/--database --query; loopback is safe
post-start — the only listener there is the real server), TabSeparated output.
`connection_string()` = clickhouse://user:pass@host:native_port/db (percent-encoded;
clickhouse-go form); `http_url()` = plain http base (credentials per request). No hooks →
reuse hash covers everything; adopted containers keep data, init scripts not re-run. Out of
scope: clusters/replicas/Keeper, the MySQL/PostgreSQL wire-emulation ports (9004/9005, off
in the default config), typed access-management toggle (rides with_env).

**MinIO module** (2026-07-13, `modules::MinIOImage` → `modules::MinIOContainer`, wave 2) —
pinned `minio/minio:RELEASE.2025-09-07T16-13-09Z`, and the pin is FROZEN by upstream, not by
us: MinIO stopped publishing community images 2025-10 and put the community edition in
maintenance mode, so this is the final tag — there is no routine bump, and it predates the
source-only CVE-2025-62506 fix (privilege escalation requiring valid credentials; a
non-issue for a throwaway test server on ephemeral localhost ports — documented, with
with_image as the escape to a patched rebuild). Ports 9000 (S3) + 9001 (console); the module
OWNS cmd `server /data --console-address :9001` because without the flag the server picks a
RANDOM console port each boot — fixing it is what makes exposing 9001 possible (a customizer
replacing the cmd is documented to orphan console_url()). Credentials minioadmin/minioadmin —
the house test/test is impossible: the server hard-rejects access keys < 3 and secret keys
< 8 chars at boot (verified live: FATAL "Invalid credentials"; also: setting only ONE of the
pair while the other is unset is itself fatal — the module always renders both), so the
render-time guard mirrors exactly those rules. DB env rule (MINIO_ROOT_USER/PASSWORD
appended LAST — the entrypoint is `sh` + `exec "$@"`, last duplicate wins; the deprecated
MINIO_ACCESS_KEY/SECRET_KEY spellings are ignored by the pinned server — doc note, no
guard). Readiness = wait_for::http("/minio/health/cluster", 9000) through the PUBLISHED
port: cluster is the ONE unauthenticated health endpoint gated on the object layer having
write quorum (on a single node: initialized and writable; 503 before), while /live and
/ready answer as soon as HTTP serves. `with_bucket` is the hook module of the pair:
post-ready, the started hook runs the in-image `mc` — and the MC_HOST_<alias> env form the
design planned DOES NOT survive real credentials (verified live: mc signs with the URL text
VERBATIM — percent-encoded creds fail auth, raw special chars break the URL parse), so the
hook uses `mc alias set tc http://127.0.0.1:9000 <key> <secret>` with creds as plain argv
(no encoding anywhere), which ALSO round-trips an authenticated call — wrong plumbing fails
start() loudly — then `mc mb --ignore-existing tc/<bucket>` per bucket. The alias persists
in the container's mc config (MC_CONFIG_DIR=/tmp/.mc baked in the image), so follow-up
test/user execs address `tc/<bucket>` directly. Hook-only state → reuse label
org.testcontainers.minio.buckets, SORTED comma-join (unlike Kafka's registration-order
topics label: bucket creation is order-independent, so the hash shouldn't churn on
registration order). Integration proof: a real S3 PUT/GET through in-image mc (cp + cat), a
host-side /minio/health/live GET, both buckets via mc ls, URL-hostile credentials
(s3cr@t/pw+8) with the hook as the positive proof and a wrong-secret `mc alias set` as the
negative, console GET / == 200. Out of scope: with_bucket policies/versioning, TLS (default
probe is plain HTTP; with_wait replaces), MINIO_DOMAIN virtual-host addressing (needs
wildcard DNS).

**RustFS module** (2026-07-13, `modules::RustFSImage` → `modules::RustFSContainer`, wave 2) —
pinned `rustfs/rustfs:1.0.0-beta.8` (Apache-2.0 MinIO alternative in Rust; NO stable line
exists yet — the newest beta by deliberate policy, re-checked against Docker Hub at
implementation: beta.8 is still newest, `latest` points at it; pin moves with upstream
betas until 1.0.0 GA, then freezes on GA; `-glibc` twins exist, default tags are
Alpine/musl). The MinIO getter-vocabulary SIBLING on purpose — host()/s3_port()/
console_port()/access_key()/secret_key()/s3_url()/console_url(), with_access_key/
with_secret_key — swapping the image class is the whole migration (the wave-2 design docs
disagreed with each other here: minio.md sketched kPort/port()/s3_url(), rustfs.md
kS3Port/s3_port()/s3_endpoint(); implementation aligned BOTH on kS3Port/s3_port()/s3_url() —
two-port modules get qualified port names, and url is the house URL-getter suffix). No
shared S3-family base class: the modules share vocabulary, not contract (different env,
health path, command, validation) — revisit only if a third S3 module lands. Ports 9000
(S3) + 9001 (console), commandless boot (entrypoint launches against baked
RUSTFS_VOLUMES=/data). Credentials rustfsadmin/rustfsadmin (the image default — the
zero-config path survives another env-handling regression; the beta line ONCE ignored the
credential env, issues #375/#1058), DB env rule (RUSTFS_ACCESS_KEY/SECRET_KEY appended
LAST; entrypoint is `sh` + `exec "$@"`). Emptiness-only render guard — verified live: the
server imposes NO length rules (1-char keys boot AND authenticate), unlike sibling MinIO.
Readiness = wait_for::http("/health", 9000) (the project's own compose healthcheck path;
answers 200 with a storage/IAM/lock ready report), chosen over the log-banner wait other
ports use — log text is the least stable API a beta has. Console quirk verified live: 9001
answers 403 at `/`, the UI lives under /rustfs/console/ — console_url() INCLUDES the
prefix. No hooks → no reuse label; no with_bucket (the image ships curl + sh but no S3 CLI,
and SigV4-by-hand is not test-helper material — an SDK client creates buckets in one call).
Integration proof: /health 200 + "ready":true and anonymous GET / == 403 host-side (the
auth layer answering is more than /health proves), console 200 under the prefix, and the
credential canary ACROSS a user-defined network — the sibling MinIO module's mc, aliased at
http://s3:9000 (`mc alias set` round-trips an authenticated call), accepts the custom pair,
creates a bucket, and rejects a wrong secret; the cross-module test doubles as the
network-alias proof. Out of scope: with_bucket (above), TLS (with_wait replaces the plain
HTTP probe), distributed/multi-volume layouts.

**ScyllaDB module** (2026-07-13, `modules::ScyllaDBImage` → `modules::ScyllaDBContainer`,
wave 2) — pinned `scylladb/scylla:2026.1` (yearly LTS line, floats over patches; bump
roughly yearly when the next x.1 LTS has patch releases behind it). LICENSE re-verified
from the primary source before implementation (the user's go condition): the ScyllaDB
Software License Agreement v1.1 (2026-04-12) is source-available, free tier 10TB/50 vCPUs
PER ORGANIZATION (a test container is orders of magnitude inside), CI/automated-testing
use EXPLICITLY permitted for commercial ScyllaDB customers ("exclusively for
non-production purposes, including Continuous Integration (CI), automated testing"), and
the library ships no ScyllaDB code, so the license binds only the image-pulling user —
same posture as mongo:7's SSPL; prohibitions are SaaS/DBaaS-offering-shaped and cannot
reach a test container. Docs carry a License note + the `with_image("scylladb/scylla:6.2")`
AGPL fallback (last OSS release, same entrypoint contract). Port 9042 only — 19042
(shard-aware) stays unexposed because shard selection is by client SOURCE port, which
Docker NAT rewrites (degrades to 9042 behavior; customizer recipe documented). Config is
FLAGS, not env (the entrypoint's argparse): managed CI shape
`--developer-mode=1 --overprovisioned=1 --smp 1 --memory 512M` rendered first, user
`with_command_args` appended after — the parser keeps the LAST occurrence, so the user
wins (Kafka rule; no credential getters to desync — the module is authless by design:
PasswordAuthenticator creates its superuser ASYNC after CQL comes up, a second
eventually-consistent phase for zero test value). `--dc` belongs to with_datacenter (raw
duplicate desyncs the getter; verified live: `--dc tcdc` boots WITHOUT `--rack`, rack
defaults to rack1). Readiness = ordered pair: wait_for::log("Starting listening for CQL
clients") — printed only after node init; on 2026.1 it appears TWICE (9042 non-shard-aware
+ 19042 shard-aware), times=1 fires on the first — then
successful_shell_command(`cqlsh "$(hostname)" -e "SELECT release_version FROM
system.local"`). `$(hostname)` is regression-proofing: the entrypoint binds CQL to the
CONTAINER IP, and the bundled cqlsh's default host regressed against that once
(scylladb#16329; plain cqlsh DOES work again on 2026.1 — verified — but the explicit form
survives both states). The cqlsh plumbing passes CQL as a POSITIONAL shell parameter
(`sh -c 'cqlsh "$(hostname)" -e "$1"' cqlsh <cql>`) — no quote escaping anywhere, verified
with quoted literals. 120s ctor budget (first boot initializes the data dir; observed 5-8s
locally in dev mode). Init scripts: NO initdb.d in the image — .cql files stage to
/tmp/testcontainers-NNNN-<name> (module-local staging; ModuleDetail's is initdb.d-shaped)
and a started hook runs `cqlsh -f` per file in registration order post-ready; a failing
script fails start(); each statement bounded by cqlsh's own request timeout; hook inputs
are the copy-to descriptors, already reuse-hash-covered → NO reuse label needed. Keyspace
contract for tests/docs: 2025.x+ enables TABLETS for fresh keyspaces, which reject
SimpleStrategy — NetworkTopologyStrategy everywhere (RF=1 warning on stderr is normal).
exec_cql output is cqlsh's aligned table — substring asserts ("(1 rows)"), not exact
matches. Out of scope: auth opt-in (with_command_args + with_wait recipe), Alternator
(DynamoDB API, port 8000), multi-node clusters (break host-side driver discovery),
family extraction with a future Cassandra module (design flat now, extract with both on
the table — the MySqlFamily lesson).

## Compose & Windows

**Docker Compose** (`DockerComposeContainer`) — three client modes: Local (DEFAULT — shells out
to the host `docker compose` CLI, the one documented exception to the library's "no docker CLI"
rule), Containerised (a long-lived `docker:26.1-cli` ambassador with the host socket
bind-mounted), and Auto (probe, then fall back). Readiness = compose v2 `--wait
--wait-timeout` PLUS a TCP probe per `with_exposed_service` (phases budgeted separately; the
probe is the Port wait strategy's — since 2026-07-11 each attempt is bounded, so a black-holed
connect cannot overshoot the wait timeout). Compose profiles: `with_profile` (repeatable,
2026-07-11) emits top-level `--profile` flags on `up` AND the teardown `down` — a
profile-less file-driven `down` would skip the gated services, leaving them to the
label sweep (a label-reconstructed `down` — no `-f`, the containerised client —
removes them regardless). Scaling: `with_scale(service, n)` (2026-07-11) emits
`up --scale service=n`; discovery keys instances by the compose container-number
label, so `get_service_port`/`get_service_container_id` take an optional instance
number (1..n; the plain forms mean the lowest-numbered instance),
`service_instances` lists the running numbers, and `with_exposed_service` can
probe a specific instance. Scaling past 1 requires ephemeral host ports (a
fixed one collides across instances). Per-service logs (2026-07-11):
`get_service_logs` (snapshot) and `follow_service_logs` (blocking or
deadline-bounded, per-instance forms included) delegate to the Engine-API log
endpoints over the discovered container id, so they work identically under
every client mode. Unpublished ports (2026-07-11): `with_ambassador(service,
port)` starts ONE socat relay container (default `alpine/socat:1.8.0.3`,
`with_ambassador_image` overrides) on the compose project network, publishing
an ephemeral port per target; `get_service_port` transparently resolves
registered pairs to the relay. Teardown removes the relay BEFORE `down` — a
foreign endpoint would block the project network's removal. Service-level by
design (compose DNS; scaled services round-robin per connection), one shared
network per relay, and a TCP probe on a relay port proves only the relay.
Local-mode children are spawned directly (`CreateProcessW` / `posix_spawnp` — no shell) with
an explicit per-child environment block, so compose env never touches the parent process and
concurrent stacks cannot cross-contaminate. The handle follows the rule of zero: the running
project lives in an internal `ActiveStack` whose destructor IS the teardown (compose `down` +
label sweep), so moves/destruction need no hand-written member lists and a failed `start()`
(including a partial `up`) cleans up after itself. Crash-safe reaping: `start()` registers an
extra `com.docker.compose.project=<project>` filter with the session's Ryuk before `up` (the
compose CLI's resources carry that label, not ours), so a crashed process's stack — containers,
project networks/volumes — is swept; with Ryuk disabled only the in-process teardown applies.

**Windows containers** — engine-mode detection (`server_os()` /
`is_windows_engine()`, cached process-wide), free-form `with_platform`, Ryuk skipped on the
Windows engine (RAII + AutoRemove only — testcontainers-dotnet parity). copy-to accepts
drive-rooted targets: `C:\dir\x.txt` is normalized to the `/dir/x.txt` form the daemon
extracts relative to `C:\` (plain `/x.txt` targets keep working). The nanoserver test image
tag is host-build-locked (`ltsc2025` on build 26100;
`tcit::WindowsEngineTest` in tests/integration/WindowsEngine.hpp resolves it from the daemon).

**Isolation (`with_isolation`)** — `HostConfig.Isolation` ("process"/"hyperv"), Windows daemons
only. Docker Desktop defaults Windows containers to Hyper-V isolation, under which the daemon
rejects filesystem operations against a RUNNING container (`copy_to`/`read_file` → HTTP 500)
and hides host DNS quirks; the Windows integration suites pin "process" (valid because the
image build is host-matched). Not set = daemon default; never send it to a Linux daemon.

**Windows-engine test mirrors** — WindowsBuildImage / WindowsVolumes / WindowsNetworks /
WindowsExec / WindowsCopy / WindowsPortGetters / WindowsWaitStrategies / WindowsLifecycle
fixtures live NEXT to their Linux twins in the same test files. Port publication needs no
in-container listener (nanoserver suffices); the listening_port wait uses a PowerShell
TcpListener in build-matched servercore (pre-cached on GitHub windows runners).
Windows-daemon facts they encode (all verified live): (a) archive uploads (`PUT .../archive`,
`docker cp` alike) land in the container LAYER and silently bypass volume mounts — which is
why `Volume::populate` seeds Windows volumes by staging into the created (not yet started)
helper's layer and
xcopying from inside (2026-07-11); (b) whether HNS serves single-label DNS names to process-isolated
containers is environment-dependent (a host DNS-suffix search list breaks it), so the network
tests assert daemon-side registration (DNSNames/Aliases in inspect) + ICMP to the peer's
network IP instead of in-container name resolution; (c) nanoserver's volume-dir ACLs require
`ContainerAdministrator`; (d) exec-stdin has no `cat` — pipe a script into `cmd /q`; (e) a
Windows Dockerfile RUN needs `USER ContainerAdministrator` to write to `C:\`; (f) an EXEC as
the default ContainerUser is denied `C:\`-root writes on ltsc2022 under process isolation
(ltsc2025 allows it; a detached exec surfaces no error, the file just never appears), and its
`%TEMP%` resolves to `C:\Windows\TEMP`, whose ACLs deny reading a just-written file back —
exec-written test files go under `%USERPROFILE%`.

**TTY containers** — `with_tty()` sets `Tty=true`; `logs()` / `follow_logs()` and the log wait
handle the raw stream automatically (`Container` remembers its TTY-ness). No interactive
attach loop (exec's stdin path is the interactive tool).

## Packaging

**Conan package** (2026-07-05) — `conan create .` produces a consumer-grade package:
`test_package/` proves find_package + link + run from a downstream project (no daemon
needed), the recipe runs the unit suite via ctest unless `tools.build:skip_test` (the
integration suite is not even compiled: the recipe pins `TC_BUILD_INTEGRATION_TESTS=OFF`,
2026-07-10), and a
CI job creates the package on Linux/Windows/macOS (macOS coverage lives ONLY there).
The version is written once, in CMakeLists' `TC_VERSION_FULL`: `project()` takes the
numeric prefix, `version.cpp` compiles the full string in, `set_version()` parses it out.
Consumers get `find_package(testcontainers)` + `testcontainers::testcontainers` on both
paths — Conan (CMakeDeps-generated configs; the installed lib/cmake is deleted from the
package to not shadow them) and plain `cmake --install` (installed package config; its
find_dependency calls prefer CONFIG mode because the exported target links Conan-named
dep targets). `shared` is validated away on Windows — the sources carry no export macros.
The ConanCenter-shaped recipe (2026-07-05) is staged verbatim in
`packaging/conan-center/` — release-tarball sources pinned by sha256, no forced
dependency options (verified against a fully compiled default Boost on gcc, msvc and
clang-15), compiler floor gcc 12 / clang 15 / apple-clang 15 / msvc 193, and
`cpp_info.requires` scoping consumers to `boost::headers` instead of the umbrella.
Submission process and the recipe-vs-recipe split are documented in its README.

**Release workflow** (2026-07-10, `.github/workflows/release.yml`) — pushing a `v*` tag
verifies the tag matches `TC_VERSION_FULL`, runs `conan create` (with the unit suite) from
the tagged tree, computes the tag-tarball sha256, and creates a DRAFT GitHub Release whose
notes carry the sha256 plus a ready `conandata.yml` entry for the ConanCenter version bump.
Publishing stays a human decision — the workflow never publishes the draft.
