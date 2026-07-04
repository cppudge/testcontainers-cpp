# TODO / Backlog

Actionable work only. What is already implemented — and its known, ACCEPTED limits — is
documented in [docs/06-feature-notes.md](docs/06-feature-notes.md); an item leaves this list
when it lands (adding a short note there if it needs one).

## Next candidates
- (empty — pick from Tech debt / Open below)

## Tech debt
- **CI analysis follow-ups** (from the 2026-07-05 review of the analysis gates) — no
  install/consumer smoke job (`cmake --install` + `find_package` from a downstream project);
  no clang *compile* job (clang appears only via clang-tidy, so clang-specific warnings on
  user builds go uncaught); no macOS job despite `Process.cpp` carrying macOS/BSD `environ`
  portability code. `TC_WERROR` + unpinned runner gcc means occasional
  `-Wno-error=<warning>` maintenance when the runner image bumps the compiler.
  (`.github/workflows/ci.yml`)
- **No Docker API version pinned** — all targets are unversioned (`/containers/create`),
  relying on the daemon's default API version; behavior can shift across daemon upgrades.
  Negotiate or pin `/v1.NN`. (`src/docker/DockerClient.cpp`)
- **Error model residuals** — no `ConflictError` (dispatch on `status_code()==409`) and no
  `ContainerStartError` for non-timeout wait failures (both considered and deferred; the
  DockerError doc reserves the right to add status subtypes, so callers must not assume exact
  dynamic types of non-404 errors). `resource_id` stays best-effort (empty on parse-layer /
  generic-`request()` failures; names the primary resource on two-resource calls).
- **Transport deadline residuals** — deadlines are per-operation (idle), so a trickling peer
  can extend a response indefinitely (fine for a trusted daemon; a whole-request cap would need
  a second budget). `pull_image` keeps the default 60s idle deadline (a very large layer
  extraction could in principle stay silent longer — widen like `build` if it ever bites).
  `DockerComposeContainer`'s own TCP probe still uses a synchronous `connect` (OS-bounded).
  (`src/docker/Transport.*`, `src/WaitStrategies.cpp`)
- **Ryuk gaps** — images never get the session label (future image resources must be tagged to
  be reaped); no graceful in-process reaper shutdown (relies on process-exit closing the
  socket); the process-global reaper binds to the FIRST daemon it starts against — a second
  daemon used later in the same process gets labels but no crash-safe reaping (a per-daemon
  reaper map would be the full fix). A real Windows Ryuk (named-pipe mount + Windows reaper
  image) is unexplored — see docs/04. (`src/Reaper.*`)
- **Log path costs** — the log wait re-fetches the full `tail=all` snapshot every 200ms;
  reimplement on `follow_logs` (scan chunks incrementally, stop via the consumer).
  `LogDemuxer::feed` does `pending_.erase(0, pos)` per chunk — O(n²) on many small chunks;
  switch to a consumed offset / ring buffer. Wait probes open a fresh TCP connection +
  `io_context` per probe (fine at 200ms polling; noted in case frequency increases).
  (`src/WaitStrategies.cpp`, `src/docker/LogDemux.cpp`)
- **follow_logs / streaming exec are blocking, cooperative-stop only** — the consumer must
  return false to stop, and only when a next chunk arrives. A background-thread RAII log handle
  with socket-level cancellation is not provided. (`src/docker/DockerClient.cpp`)
- **exec residuals** — no TTY resize (`POST /exec/{id}/resize`); one fresh connection per exec;
  stdin is written fully (then half-closed) BEFORE any output is read, so a command echoing a
  LARGE stdin back can backpressure the write into an io-deadline timeout (bounded, not a hang;
  realistic exec-stdin payloads are tiny) — interleave the stdin write with the output read if
  it ever matters. Named-pipe half-close note: `FlushFileBuffers` before the zero-length EOF
  message is the one transport operation the io deadline cannot bound (go-winio parity).
- **`run_process` residuals** — children are spawned directly (no shell), so shell builtins
  and `.bat`/`.cmd` scripts are not runnable (every caller passes a real executable: docker,
  compose, docker-credential-<helper>). On POSIX, `working_dir` needs
  `posix_spawn_file_actions_addchdir_np` (glibc 2.29+ / macOS; throws elsewhere — callers pass
  nullopt today). Output is fully buffered until child exit (no streaming consumer). On
  Windows, narrow→wide conversion uses CP_ACP (parity with `path::string()`), so argv/env/paths
  not representable in the ANSI code page won't round-trip — switch to UTF-8 end to end if it
  ever bites. (`src/Process.cpp`)
- **`server_os()` cache race is benign but unrealized** — the mutex is released between the
  cache read and the `GET /version`, so two threads can both issue the first request
  (idempotent, harmless). `std::call_once` would realize the double-checked intent.
  (`src/docker/DockerClient.cpp`)
- **Credential helpers** — output is not cached (the helper is re-invoked on every pull,
  alongside the per-pull config re-read); no end-to-end private-registry integration test
  against a real authenticated registry. (`src/docker/Auth.cpp`)
- **copy-to / copy-from** — USTAR caps entry path length (100/255 chars; pax would lift it);
  one tar + PUT per source (no batching); no directory-tree copy-from helper (use
  `extract_tar` on the raw bytes).
- **Build from Dockerfile** — no `.dockerignore` filtering on `with_file` directory walks;
  build output is fully buffered (no live streaming consumer — could reuse the `follow_logs`
  chunked-read approach); built images are not session-labeled (not reaped). No secrets / ssh /
  cache-from / squash / platform-on-build.
- **HostConfig typed setters** — cpu limits, restart policy, dns, sysctls, devices, pids-limit
  still go through the `with_create_body_patch` escape hatch; `ContainerInspect` doesn't
  surface Memory/CpuQuota/etc., so those can't be asserted via inspect.
- **Networks** — no network inspect; IPAM supports a single Subnet/Gateway pair (no multiple
  pools / IPRange / aux addresses); no process-wide dedup (each `create()` makes a new network).
- **Volumes** — no `list_volumes` / prune / anonymous-volume management; `populate` spins up a
  real helper container per call (no batching).
- **Compose gaps** — `--profile`, service scaling (`--scale`), per-service log streaming, a
  socat ambassador for UNPUBLISHED ports, and Ryuk-reaping of compose containers (they carry no
  session label) are all unsupported.
- **Windows containers** — copy-to Unix-normalizes entry paths, so `C:\...` targets aren't
  handled (use `/x.txt` = `C:\x.txt`). `Volume::populate` cannot seed a Windows volume — the
  daemon extracts archives into the container LAYER, bypassing mounts (`docker cp` shares the
  blind spot); a Windows seeding mechanism would need a stage-then-in-container-copy helper.
  Remaining Windows-mode test gaps: the http wait (needs a real HTTP server image —
  the PowerShell TcpListener in servercore covers listening_port only), bind mounts, and
  the stopping hook (see docs/07 for the full matrix).
- **Host resolution** — docker-context TLS materials (the context can carry ca/cert/key paths)
  are not consumed, only the `Host` endpoint; only `docker.host` is read from
  `~/.testcontainers.properties`. (`src/docker/HostResolve.hpp`)
- **Image substitution scope** — the substitutor applies at the `GenericImage` layer only;
  `GenericBuildableImage` / Compose / raw `DockerClient` calls are not substituted. No
  time-based ("pull if older than N") policy; `Always` re-pulls on every `start()`.
- **Test gaps** — the WaitStrategies duration-clamp-to-deadline branch is only exercised via
  the integration `WaitStrategiesTest` (extract a pure `clamped_wait_plan(now, value, deadline)`
  if it ever regresses).
- **Host access residuals** — libssh2 landed as a HARD dependency (the planned optional
  `find_package` guard would make the public API compile-time-conditional; revisit if the
  dependency ever bothers a consumer); the sidecar/tunnel singleton binds to the FIRST daemon
  used (same shape as the Ryuk residual); remote forwards are never cancelled once added; the
  tunnel pump wakes every 100ms even when idle (fine for test traffic).
  (`src/HostPortForwarding.cpp`)
- **msvc-preset configure noise** — non-fatal `IMPORTED_LOCATION ... _DEBUG ... Release`
  messages for OpenSSL/zlib under the Visual Studio preset (Conan installs Release-only); the
  default `ninja` preset is unaffected.

## Open / not yet built
- **Full connection pool (option B)** — deferred until a real remote-TCP/TLS use case appears;
  scoped keep-alive sessions cover the polling hot path today (decision record in docs/06).
  Parameters to build it with: endpoint-keyed shared pool behind a `shared_ptr`, ~90s idle TTL,
  4–8 idle per endpoint, retry-once-only-on-unsent, streaming excluded.
- **TLS end-to-end in CI** — the transport is implemented and the cert resolution unit-tested,
  but `TlsTransportTest` needs a reachable remote TLS daemon (skipped otherwise), so the path
  is unproven end to end.

> Implemented milestones (container config, wait strategies, exec, networks, volumes, compose,
> reuse, hooks, Ryuk, auth, build-from-Dockerfile, …) are documented with their known limits in
> [docs/06-feature-notes.md](docs/06-feature-notes.md).
