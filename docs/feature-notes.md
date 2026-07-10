# Feature notes — what is implemented, and its known limits

Short reference notes on the implemented subsystems, distilled from the backlog as items were
completed. This file records what EXISTS, where it lives, and the caveats a user or maintainer
should know; the actionable leftovers live in [TODO.md](TODO.md).

## Transport & client

**Transports** (`src/docker/Transport.*`) — unix socket / Windows named pipe / TCP / TLS behind
one `ITransport`; `connect()` picks by scheme. TLS materials resolve from `DOCKER_CERT_PATH`
(falling back to `~/.docker` under `DOCKER_TLS_VERIFY`) via the pure `TlsConfig` helpers
(unit-tested). Mutual TLS is verified end to end in CI against a real `--tlsverify` daemon
(the `tls-e2e` docker:dind job; fixed 2026-07-10 — the SSL context used to be configured
after the stream was created, so the client certificate was never presented and server
verification silently stayed fail-open; the ordering is pinned by two in-process
TlsTransportTest cases). The TLS transport is a build option (CMake `TC_TLS` / conan `tls`,
default ON) — the library's only direct OpenSSL use; with it off, `connect()` for an
`https://` host throws a `DockerError` naming the option, and the pure `TlsConfig` helpers
stay available. Note the daemon-host spelling difference: this library takes
`https://host:port` directly, while the docker CLI spells the same thing
`tcp://host:port` + `DOCKER_TLS_VERIFY` (a `tcp://` → TLS upgrade is not implemented —
see TODO's host-resolution entry).

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
numerically ("1.9" < "1.44"). 1.44 covers everything the library uses (newest need:
`?platform=` on create, 1.41) and is the negotiation floor of daemons that dropped the old
versions; an old daemon wins with its own version. Copies inherit the negotiated version (the
drop-time DELETE of a `Container` handle re-uses it — no second ping); a daemon that reveals no
parsable version falls back to unversioned paths (its default version, the old behavior). The
raw `request()` escape hatch stays deliberately unversioned. Residual: negotiation is lazy
per-instance state — same "one instance, one thread" rule as sessions.

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

**Reaper (Ryuk)** — containers, networks, named volumes, and built images carry the session
label; compose stacks are covered by an extra per-project filter registered over the same
control connection (each line ACKed). Pinned to `testcontainers/ryuk:0.11.0`, skipped on the
Windows engine and via
`TESTCONTAINERS_RYUK_DISABLED`. Per-daemon: reapers are keyed by endpoint URL, so
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
listening port, run in order under one shared startup timeout; the inspect-based polls run
inside a `DockerClient::Session`. The alternative TYPES live in `wait_for::` next to the
factories (2026-07-05, pre-0.1.0 rename): the former `testcontainers::wait` namespace was
ambiguous against POSIX `::wait(2)` under `using namespace testcontainers;` on macOS. `wait_for::listening_port` probes the published HOST port
only (no in-container listen check), so a port published before the process binds could read
ready early. HTTP/TCP probes are deadline-bounded per probe (min(time left, 5s), which absorbs
Windows' ~2s refused-SYN retry on the dead `::1` half of "localhost").

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
Stdin EOF:
TCP / unix sockets half-close via `shutdown(send)`; the Windows named pipe mirrors go-winio's
`CloseWrite` (flush, then a zero-length message — message-mode pipes only, which is what every
real daemon serves; inside the pump the flush also pauses output reads while it blocks); TLS
cannot half-close, so exec-with-stdin throws up front there (Go's `tls.Conn` cannot either —
the docker CLI hangs where we throw).

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
env, cmd, entrypoint, exposed ports, workdir, user); the instance form uses `image():tag()`
verbatim, no substitutor. Built images are session-scoped: `build()` ships the managed-by /
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

**Networks** — `Network` RAII handle + builder (driver / internal / attachable / IPv6 / one
IPAM subnet+gateway pair / driver options / labels); `GenericImage::with_network` takes the
handle directly or a name string (the handle overload records `name()` — ownership stays with
the handle); `with_network_alias` (per-network DNS
aliases); `with_static_ipv4` (a fixed endpoint address — needs a user-defined network whose
subnet contains it); `Network::connect` attaches a running container. Inspect: `net.inspect()`
/ the static `Network::inspect(name_or_id)` return a typed `NetworkInspect` (driver, scope,
internal/attachable/IPv6 flags, IPAM pools, options, labels, and the attached containers'
endpoints — addresses in CIDR form), `net.inspect_raw()` the full JSON (both over
`DockerClient::inspect_network[_raw]`). `net.keep()` releases removal ownership —
`Container::keep` semantics, including the Ryuk caveat (the network stays session-labeled, so
the reaper still removes it after the process exits) and the `keep(false)` re-arm. No
process-wide dedup — every `create()` makes a brand-new network.

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
container's mountpoint). No list/prune; the RAII drop fails (409) while a container still
mounts the volume — tear down in reverse-declaration order.

**Image pull policy + name substitution** — `ImagePullPolicy::Default` (lazy: pull on a create
404) / `Always` (pull before every create); substitution via
`TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX` or a custom `with_image_name_substitutor` (replaces the
default). Applied at the `GenericImage` layer only — not for builds, compose, or raw
`DockerClient` calls.

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

## Compose & Windows

**Docker Compose** (`DockerComposeContainer`) — three client modes: Local (DEFAULT — shells out
to the host `docker compose` CLI, the one documented exception to the library's "no docker CLI"
rule), Containerised (a long-lived `docker:26.1-cli` ambassador with the host socket
bind-mounted), and Auto (probe, then fall back). Readiness = compose v2 `--wait
--wait-timeout` PLUS a TCP probe per `with_exposed_service` (phases budgeted separately).
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
`docker cp` alike) land in the container LAYER and silently bypass volume mounts, so
`Volume::populate` is Linux-only — seed Windows volumes by exec'ing writes in a container
that mounts them; (b) whether HNS serves single-label DNS names to process-isolated
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
