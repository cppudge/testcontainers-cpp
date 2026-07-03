# TODO / Backlog

Running list of known limitations, tech debt, and future work. Items found during
review are recorded here so they aren't lost between milestones.

## Known limitations / tech debt
- **No I/O timeouts in the transport layer** — every transport does synchronous blocking
  `read_some`/`write_some`/`resolve`/`connect` on a private `io_context` with no deadline, so a
  wedged daemon, a stuck exec, or a network black-hole blocks the calling thread forever. The
  `ITransport` doc-comment lists timeouts as an expected concern, but nothing implements them.
  Fix direction: per-operation deadlines (Asio `async_*` + `io_context::run_for`, or a
  steady-timer that cancels the socket) exposed through `ITransport`. (`src/docker/Transport.*`)
- **Error model thinner than the README claims** — README promises "structured errors carrying
  HTTP status / container id", but `Error`/`DockerError` are bare `runtime_error`s with no
  fields or subtypes. The same `DockerError` is also thrown for pure usage errors (port not
  exposed, readiness timeout), so callers cannot `catch` selectively. Fix direction: a small
  hierarchy (e.g. `TimeoutError`, `NotFoundError`) + status/id fields on `DockerError`.
  Related: the create-endpoint "Id"/"Name" extraction now goes through
  `docker::expect_string_field` (wraps nlohmann failures in DockerError), but the other parse
  entry points (`parse_inspect`, `parse_container_list`, `parse_volume_inspect`,
  `parse_server_os`, `parse_exec_exit_code`) still call `nlohmann::json::parse` unguarded — an
  HTML error page through a 200 escapes as a raw `json::parse_error`; route them through the
  same wrap-to-DockerError policy. (`include/testcontainers/Error.hpp`, `src/docker/ApiMapping.*`)
- **`run_process` env save/apply/restore is not thread-safe** — local-mode compose (and the
  credential-helper path) mutate process-global env via `_putenv_s`/`setenv` around the child
  run; two compose stacks torn down concurrently (destructors on different threads) can
  cross-contaminate env. Serialize with a static mutex, or move to `CreateProcessW`/`posix_spawn`
  with an explicit environment block. (`src/Process.cpp`)
- **`Process.cpp` command-line construction is untested and Windows-quoting is fragile** —
  `quote_arg` backslash-escapes embedded `"`, which cmd.exe does not honor (POSIX/MSVCRT-argv
  convention only); currently safe because all input is library-controlled, but it is the
  riskiest code in the repo and has zero unit tests (functions are file-local). Fix direction:
  extract `build_command_line`/`quote_arg` into a testable internal header + unit tests
  (spaces, embedded quotes, the cmd.exe outer-wrap). (`src/Process.cpp`)
- **No Docker API version pinned** — all targets are unversioned (`/containers/create`), relying
  on the daemon's default API version; nothing negotiates or pins `/v1.NN`. Behavior can shift
  across daemon upgrades. (`src/docker/DockerClient.cpp`)
- **`server_os()` cache race is benign but unrealized** — the mutex is released between the
  cache read and the `GET /version`, so two threads can both issue the request on first use
  (idempotent, harmless). `std::call_once` or holding the lock across the miss would realize
  the double-checked intent. (`src/docker/DockerClient.cpp`)
- **Test gaps found in review** — `DockerComposeContainer` move-ctor/assign have no daemon-free
  unit test (the hand-written moves zero `started_`/`stopped_`/`temp_file_`); pure
  `properties_reuse_enabled()` parsing is untested (`tests/unit/ReuseTest.cpp` skips it);
  the containerised compose `exec_compose` shell-quoting has no test for env values with
  quotes/spaces; WaitStrategies' `count_occurrences` and the Duration-clamp branch are only
  exercised via integration.
- **`LogDemuxer` streaming cost** — `feed()` does `pending_.erase(0, pos)` on every
  call: O(n) per chunk → O(n²) for many small chunks. Fine for `demux_all` (single
  feed); switch to a consumed-offset / ring buffer when the follow/streaming path
  lands. (`src/docker/LogDemux.cpp`)
- **TTY containers supported (raw log path)** — `GenericImage::with_tty()` sets top-level
  `Tty=true`; `LogOptions.tty` selects a raw/unframed decode path so `logs()` / `follow_logs()` skip
  `demux_all` (a TTY stream has no 8-byte frame header and no separate stderr channel). The `Container`
  handle remembers its TTY-ness (`has_tty()`) and the log-wait honors it (`wait_until_ready(..., tty)`),
  so `wait_for::log` works on a TTY container. `ContainerInspect.tty` surfaces `Config.Tty`. Known
  limits: (a) a pseudo-TTY emits `\r\n` line endings (cooked mode) — match substrings, not exact bytes;
  (b) no interactive attach loop (that's the exec stdin/hijack path); (c) the low-level
  `DockerClient::logs`/`follow_logs` need `opts.tty` set by the caller — only the high-level `Container`
  sets it automatically. (`src/docker/DockerClient.cpp`, `include/testcontainers/docker/Logs.hpp`)
- **lifecycle hooks + startup retry** — `GenericImage` has `with_created_hook`/`with_starting_hook`/
  `with_started_hook`/`with_stopping_hook` (`LifecycleHook = std::function<void(DockerClient&, const
  std::string&)>`, so a hook gets the full low-level client + id) and `with_startup_attempts(n)`
  (retries the whole create→start→wait, removing each failed partial; the reuse-ADOPT path is not
  retried). created/starting/started fire inside `start()`'s try (a throwing hook cleans up the partial
  container); stopping fires once from `Container` on `stop()`/auto-removing `drop()` (never on a
  persistent/reusable handle), swallowing exceptions in the noexcept teardown. Known limits / one-line
  notes: (a) no distinct `containerIsStopped` (only "stopping"), and stopping does NOT fire for a
  reused/persistent handle's drop; (b) no inter-attempt backoff/delay (immediate retry); (c) hooks get
  `(DockerClient&, id)` — there is no richer per-container facade (use the client's exec/copy/inspect by
  id). (`include/testcontainers/Lifecycle.hpp`, `src/GenericImage.cpp`, `src/Container.cpp`)
- **follow logs: blocking + cooperative-stop only** — `DockerClient::follow_logs` /
  `Container::follow_logs` stream incrementally (`read_some` + `LogDemuxer`) and stop when
  the consumer returns false, but it is BLOCKING: run it on your own `std::thread` for
  background consumption. A background-thread RAII log handle with socket-level cancellation
  (stop even when no new bytes arrive) is not provided yet. (`src/docker/DockerClient.cpp`)
- **log-wait still polls snapshots** — the log wait in `src/WaitStrategies.cpp` re-fetches the
  full `tail=all` snapshot every 200ms; it could now be reimplemented on `follow_logs` (scan
  chunks, return false from the consumer once the substring count is reached).
- **TLS (https) transport implemented** — `connect()` returns a `TlsTransport` (Asio
  `ssl::stream`) for the `https://` / `tcp+tls` scheme; TLS materials (ca/cert/key) resolve from
  `DOCKER_CERT_PATH` (falling back to `~/.docker` when `DOCKER_TLS_VERIFY` is set) via the pure
  `TlsConfig` helpers. The cert-resolution logic is unit-tested (`TlsConfigTest`); the end-to-end
  `TlsTransportTest` needs a reachable remote TLS daemon (skipped otherwise), so it is not exercised
  in CI. (`src/docker/Transport.cpp`, `src/docker/TlsConfig.hpp`)
- **Docker host resolution: full order, endpoint-only** — `DockerHost::resolve` now does the
  testcontainers order (first hit wins): `DOCKER_HOST` → `docker.host` in
  `~/.testcontainers.properties` → active docker context (`DOCKER_CONTEXT`/`currentContext`/`default`,
  reading `Endpoints.docker.Host` from `~/.docker/contexts/meta/<sha256(name)>/meta.json`) → default
  socket with rootless fallbacks (`$XDG_RUNTIME_DIR/docker.sock`, `$HOME/.docker/run/docker.sock`,
  else `/var/run/docker.sock`; Windows named pipe). Pure parsers + a vendored SHA-256 (no OpenSSL)
  live in `src/docker/HostResolve.hpp` + `DockerHost.cpp`; steps 2-4 never throw on a malformed file.
  Known limits / one-line notes:
  (a) docker-context TLS materials (the context can carry ca/cert/key paths) are NOT consumed — only
  the `Host` endpoint;
  (b) `~/.testcontainers.properties` — only `docker.host` is read (not `tc.host` or other props);
  (c) `DOCKER_TLS_VERIFY` / `DOCKER_CERT_PATH` ARE handled now — by the TLS transport
  (`src/docker/TlsConfig.hpp`, see the TLS item), not by host resolution itself.
  (`src/docker/DockerHost.cpp`, `src/docker/HostResolve.hpp`)
- **One connection per request** — `request()` opens/closes a transport each call
  (no keep-alive / pooling).
- **port + inspect getters (done, uncached)** — `Container` now has `get_host_port` (IPv4-preferred),
  explicit `get_host_port_ipv4`/`get_host_port_ipv6` (throw if that family isn't published),
  `first_mapped_port()` (the FIRST exposed port via the order recorded by `start()`, else the
  lowest-numbered published port), `inspect()` (structured `ContainerInspect`) and `inspect_raw()` (the
  full inspect JSON string for fields we don't model). The binding-selection logic is a pure, unit-tested
  helper (`src/docker/Ports.hpp`: `select_host_port`/`lowest_published_host_port`). Known limits:
  (a) every getter re-inspects the container (no caching of the published ports);
  (b) `first_mapped_port`'s exposed-order is only known for handles
  returned by `start()` (adopted/manual handles fall back to lowest-numbered).
  (`src/Container.cpp`, `src/docker/Ports.hpp`)
- **Log-wait polling cost** — the log wait re-fetches the full `tail=all` snapshot
  every 200ms; switch to an incremental follow-stream scan (ties to the follow-logs
  item above). (`src/WaitStrategies.cpp`)
- **Wait probes open a fresh TCP connection + `io_context` per probe** — fine for ~200ms
  polling; noted in case probe frequency ever increases. (`src/WaitStrategies.cpp`)
- **`wait::Port` probes only the externally mapped host port** — `wait_for::listening_port` resolves
  the published host port and does a TCP connect; it does NOT do the in-container `/proc/net/tcp`
  listening check that testcontainers-java additionally performs (`tcp_probe` in
  `src/WaitStrategies.cpp`). Adequate for "is the service reachable from the host", which is what tests
  need, but a container whose port is published before the process binds could read as ready early.
- **msvc-preset configure noise** — under the Visual Studio (multi-config) preset, CMake prints
  non-fatal `IMPORTED_LOCATION ... _DEBUG ... Release` errors for OpenSSL/zlib because Conan
  installs Release-only; the Release build and tests still succeed. The default `ninja` preset is
  unaffected. (Install both configs, or filter the message, if it becomes annoying.)
- **exec: options + streaming + stdin (half-close-gated)** — `exec` now takes `ExecOptions`
  (env / working_dir / user / privileged / tty / stdin_data) and has a streaming overload
  (`exec(cmd, opts, LogConsumer)`, incremental `read_some` + `LogDemuxer`, consumer returns false to
  stop; the result's stdout/stderr are empty — output went to the consumer). `tty=true` reads the raw
  unframed stream into `stdout_data` (stderr empty). Known limits / one-line notes:
  (a) **stdin only works on a half-closable transport** — feeding `stdin_data` writes the bytes onto
  the hijacked connection then calls `ITransport::shutdown_send()` so the in-container reader sees EOF;
  TCP and unix sockets implement it, but the Windows **named pipe** and **TLS** transports cannot
  half-close (their `shutdown_send()` is a documented no-op), so exec **throws DockerError up front**
  on those transports (`ITransport::supports_half_close()`, checked BEFORE exec-create) instead of
  letting a reader like `cat` hang forever — `Exec.FeedsStdin` SKIPS there (so the happy path is
  unproven on this named-pipe host, though the create-body + write path is unit-tested and the
  throw is integration-tested). **The named-pipe half is FIXABLE**: moby creates the daemon pipe
  in message mode, and go-winio's `CloseWrite` signals EOF with a zero-length message
  (`WriteFile(h, buf, 0, ...)`) — exactly how `docker exec -i` works on Windows. Implementable on
  the Asio `stream_handle` via `native_handle()`; that would make exec-stdin work on the PRIMARY
  local transport, leaving the throw a TLS-only edge case (Go's `tls.Conn` has no `CloseWrite`
  either — even the docker CLI hangs there, so throwing beats the reference behavior);
  (b) no exec **TTY resize** (`POST /exec/{id}/resize`) / window size;
  (c) still one fresh connection per exec, and the streaming overload has the same blocking +
  cooperative-stop-only nature as `follow_logs` (no socket-level cancellation).
  (`src/docker/DockerClient.cpp`, `include/testcontainers/ExecOptions.hpp`, `src/docker/Transport.cpp`)
- **richer networks: builder + aliases + connect-existing, with limits** — `Network::builder()`
  exposes driver / internal / attachable / EnableIPv6 / IPAM subnet+gateway / driver options /
  labels (`build_network_create_body` in `src/docker/ApiMapping.cpp`); `GenericImage::with_network_alias`
  emits per-network DNS aliases (`NetworkingConfig`, requires `with_network`); `Network::connect`
  attaches an already-running container (`POST /networks/{id}/connect`, optional aliases). Known
  limits / one-line notes: (a) there is NO network inspect, and no connect-to-existing-by-name beyond
  the id-based `connect_network`; (b) IPAM supports a single Subnet/Gateway pair only (no multiple
  pools / IPRange / aux addresses); (c) `Network` still has no process-wide dedup — each `create()`
  (and each `builder().create()`) makes a brand-new network.
  (`include/testcontainers/Network.hpp`, `src/Network.cpp`, `src/docker/DockerClient.cpp`)
- **Ryuk coverage & lifecycle** — containers, networks, and named volumes (incl. the transient
  volume-seed helper container) now get the session-id label; **images** still don't, so future
  image resources must also be tagged to be reaped. The global `Reaper` has
  no graceful in-process shutdown (relies on process-exit closing the socket); the Ryuk container is
  `AutoRemove`d on exit. Image pinned to `testcontainers/ryuk:0.11.0`.
- **Registry credential helpers: get-only, uncached** — `resolve_auth_for_image` now resolves
  `credsStore` (global) / `credHelpers` (per-registry) by shelling out to `docker-credential-<helper>
  get` (the default on Docker Desktop), with plaintext `auths` still taking precedence
  (`select_credential_helper` / `parse_credential_helper_output` / `auth_from_credential_helper` in
  `src/docker/Auth.cpp`, via the moved `src/Process.*` with stdin-redirect support). Known limits /
  one-line notes: (a) the helper output is NOT cached — it is re-invoked on every pull, alongside the
  existing per-pull config re-read from disk; (b) only the `get` verb is used (no `store`/`erase`/`list`);
  (c) end-to-end private-registry pull via a helper still isn't integration-tested against a real
  private registry (needs a reachable authenticated registry; flaky on Docker Desktop). The smoke test
  only proves the subprocess+parse path runs without throwing.
- **copy-to-container: USTAR + one PUT per source** — `build_tar` uses USTAR, which caps entry path
  length (100 chars, 255 with prefix); very long container paths would need the pax format. Each
  `with_copy_to` source is a separate tar + `PUT .../archive` (not batched into one). The target's
  parent directory must already exist in the container.
- **copy-from-container: single-file helpers only** — `Container::read_file` / `copy_file_from` (and
  the low-level `copy_from_container` + `docker::extract_tar`) cover a single regular file. Directory-tree
  extraction from `copy_from_container` is not exposed via a high-level helper yet; use `extract_tar`
  directly on the raw tar bytes for trees.
- **build-from-Dockerfile (`GenericBuildableImage`): no .dockerignore, buffered output, unreaped
  images** — `GenericBuildableImage(name, tag)` builds from a Dockerfile + a build context of
  `CopyToContainer` entries: `with_dockerfile`(host path) / `with_dockerfile_string`(inline) for the
  Dockerfile, `with_file`(host file/dir, walked recursively — no `.dockerignore` filtering) and
  `with_data`(in-memory bytes) for context; `build()` builds the image tagged `<name>:<tag>` and
  returns a runnable `GenericImage`. `DockerClient::build_image` buffers the entire build-output
  stream (no live build-log streaming/consumer — could reuse the `follow_logs` chunked-read approach).
  Built images carry no Ryuk session-id label, so they are NOT auto-reaped (only containers/networks
  are); `with_no_cache`/`with_pull`/`with_target`/`with_build_arg` are supported, but secrets, ssh,
  cache-from, squash, and platform on build are not. (`src/GenericBuildableImage.cpp`)
- **HostConfig: typed subset + escape hatch** — `GenericImage` has typed setters for memory, shm_size,
  ulimits, cap_add/cap_drop, extra_hosts; everything else goes through `with_create_body_patch` (a raw
  `/containers/create` JSON fragment deep-merged via RFC-7386 AFTER our fields, so it overrides them; nest
  HostConfig fields under `"HostConfig"`). No typed setters yet for cpu limits, restart policy, dns,
  sysctls, devices, pids-limit (use the patch). `ContainerInspect` still doesn't surface Memory/CpuQuota/etc.,
  so those can't be asserted via inspect. (`src/docker/ApiMapping.cpp`)
- **Windows containers: dotnet-parity only** — the engine mode is detected (`is_windows_engine()`) and
  Ryuk is skipped on the Windows engine, so there is **no crash-safe reaping** on Windows (RAII /
  AutoRemove only), matching testcontainers-dotnet. `copy-to-container` still Unix-normalizes the entry
  path, so `C:\...` targets aren't handled yet. Wait strategies are OS-agnostic (no PowerShell
  command-wait). The nanoserver test image tag is host-build-locked (`ltsc2025` on build 26100).
  A real Windows Ryuk (named-pipe mount + Windows reaper image) is unexplored — see `docs/04`.

- **image pull policy + name substitution: minimal** — `GenericImage` supports
  `with_image_pull_policy(ImagePullPolicy::Always|Default)` and
  `with_image_name_substitutor(fn)`. Known limits / one-line notes:
  (a) only the Hub-prefix env substitutor (`TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX`,
  via `docker::substitute_image_name`/`apply_hub_image_prefix`) plus a custom hook
  are provided — there is NO "pull if older than N" / time-based pull policy;
  (b) substitution is applied at the `GenericImage` layer only —
  `GenericBuildableImage` / Compose / raw `DockerClient` calls are NOT substituted;
  (c) `ImagePullPolicy::Always` re-pulls on every `start()` (no pull-pause / dedup).
  (`include/testcontainers/GenericImage.hpp`, `src/GenericImage.cpp`, `src/docker/Auth.cpp`)
- **Docker Compose: three client modes (rust parity), published-ports-only** —
  `DockerComposeContainer` now has THREE client modes (`ComposeClientKind`): Local (DEFAULT —
  shells out to the host `docker compose` CLI; the documented compose-only exception to the
  "no docker CLI" rule), Containerised (a long-lived `docker:26.1-cli` container with the host
  docker socket bind-mounted; `up`/`down` are `exec`'d in, files copied to
  `/docker-compose-<i>.yml`), and Auto (probe `docker compose version`, else fall back to
  Containerised). Readiness uses compose v2 `--wait --wait-timeout` (default 60s) PLUS the
  per-`with_exposed_service` TCP probe. Multi-file `-f`, `with_env`/`with_env_vars`,
  `with_build`/`with_pull`, `with_remove_volumes`/`with_remove_images` are supported. Discovery
  is still by the `com.docker.compose.project` label; teardown is `down` + a project-label sweep
  + RAII. Internal split: pure arg-builders in `src/compose/ComposeCommand.*` (unit-tested), a
  popen-based subprocess helper in `src/Process.*` (`testcontainers::detail`, shared with the
  credential-helper path), and the client implementations + factory in `src/compose/ComposeClients.*`.
  Known limits / one-line notes:
  (a) Local mode shells out to the host `docker compose` (the documented compose-only exception
  to "no docker CLI" — strictly inside `LocalComposeClient`);
  (b) local-mode env vars are set on the CHILD PROCESS only (saved/restored around the run via
  `_putenv_s`/`setenv`), not on the daemon or this process persistently; containerised mode
  passes them via a `/bin/sh -c "KEY=VALUE ... docker compose ..."` exec wrapper;
  (c) still unsupported: `--profile`, service scaling (`--scale`), per-service log streaming,
  socat ambassador for UNPUBLISHED ports, build contexts / host-relative volumes / `.env`
  beyond what `--project-directory` (parent of the first file, local mode) covers, and the
  compose stack is still NOT Ryuk-reaped (compose containers carry no session-id label).
  (`src/DockerComposeContainer.cpp`, `src/compose/*`)

- **reusable containers (`with_reuse`): exact-config hash, never auto-removed** —
  `GenericImage::with_reuse(true)` adopts an already-running container matching a stable
  reuse-hash label (`org.testcontainers.reuse.hash`, FNV-1a over the create body + copy-to
  descriptors); safety-gated on `TESTCONTAINERS_REUSE_ENABLE` / `testcontainers.reuse.enable=true`
  in `~/.testcontainers.properties`, degrading to a normal container otherwise. Reuse containers
  carry no session-id label (so Ryuk won't reap them) and the handle is persistent (no remove on
  drop). Known limits / one-line notes:
  (a) the reuse hash covers the create body + copy-to descriptors, but for HOST-FILE copies it
  hashes the host PATH, not the bytes — a changed file at the same path still reuses the old container;
  (b) reuse containers are NEVER auto-removed and NOT reaped — callers / CI must prune them
  (e.g. `docker container prune` or a label sweep on `org.testcontainers.reuse.hash`);
  (c) there is no "reuse enabled" marker label and no reuse-across-different-images dedup beyond
  the exact-config hash (any config difference yields a different hash → a fresh container).
  (`include/testcontainers/GenericImage.hpp`, `src/GenericImage.cpp`, `src/Reuse.cpp`,
  `include/testcontainers/Container.hpp`)
- **expose-host-ports NOT implemented (Tier 2.8, deferred by decision)** — there is no
  `Testcontainers::expose_host_ports(...)` yet (letting a container reach a service on the HOST at
  `host.testcontainers.internal:<port>`). The intended implementation mirrors testcontainers: an
  `testcontainers/sshd` sidecar container + an SSH **reverse tunnel** (`-R <port>:localhost:<port>`)
  established from this process via **libssh2** (ConanCenter), with `host.testcontainers.internal`
  resolving to the sshd container. **libssh2 should be an OPTIONAL dependency** — the feature compiles/
  links only when it's available (e.g. a CMake option / `find_package` guard), so the core library keeps
  no hard SSH dependency. Interim workaround that already works today: a container can reach the host via
  `GenericImage(...).with_extra_host("host.docker.internal", "host-gateway")` (Docker Desktop, or Linux
  20.10+ with host-gateway) — but that exposes the whole host and has no per-port control.

- **named volumes (create/inspect/remove + seed), with limits** — `Volume` move-only RAII (mirrors
  `Network`): `Volume::create()`/`create(name)` (generated `tc-<hex>`) + a `Builder`
  (driver/driver_opt/label), best-effort remove on drop, session-labeled for Ryuk. Low-level
  `DockerClient::create_volume`/`inspect_volume`/`remove_volume` + `build_volume_create_body`/
  `parse_volume_inspect`. `Volume::populate(sources, mount_path="/__tc_seed", helper_image="alpine:3.20")`
  seeds data by mounting the volume into a throwaway helper container and copying the (rebased,
  volume-relative) sources through; the helper is started before the archive PUT (portable: not every
  daemon materializes the write on the mountpoint of a created-only container) and always force-removed.
  Known limits / one-line notes: (a) NO `list_volumes` / prune, and no anonymous-volume management;
  (b) `populate` spins up + tears down a real helper container per call (no batching), pulls
  `alpine:3.20` if absent, and host-file sources hash/copy by path at call time; (c) `inspect_volume`
  surfaces only Name/Driver/Mountpoint/Scope/Labels/Options (no UsageData / status);
  (d) the volume's own RAII drop fails (409) if a container still has it mounted — tear down mounting
  containers first (the high-level handles already drop in reverse-declaration order).
  (`include/testcontainers/Volume.hpp`, `src/Volume.cpp`, `src/docker/DockerClient.cpp`)

- **`start()` orchestration welded to `GenericImage` (planned split — responsibility + testability,
  not a blocker)** — the whole container lifecycle (reaper bootstrap → reuse lookup → startup-attempt
  retry → create → copy-to → created/starting hooks → start → wait-until-ready → started hooks → handle
  construction) lives inside the private `GenericImage::start()`, reading every input off `*this`. PLAN:
  split it in two, mirroring testcontainers-rs (`Image`/`ContainerRequest` + `Runner`): (1) a
  `ContainerRequest` value type — the "what to run": the `CreateContainerSpec` plus the run-time fields
  `start()` currently reads from the builder (waits, startup_timeout, registry_auth, copy_to_sources,
  the four lifecycle-hook vectors, reuse, pull_policy, startup_attempts, declared `exposed_ports`); and
  (2) a `Runner` / free `Container run(const ContainerRequest&)` holding the orchestration (Reaper /
  Reuse / WaitStrategies stay internal `src/` helpers). `GenericImage::start()` then becomes a thin
  `return run(to_request());` — its public API is unchanged. MOTIVATION is ONLY separation of
  responsibility + unit-testability of the orchestration (drive `run()` against a fake/mock
  `DockerClient` without constructing a `GenericImage`); there is **no functional blocker** today, so
  this is a cleanliness/testing refactor, not urgent. Explicitly **NOT for modules**: future ecosystem
  modules (Postgres/Redis/…) are to be built by COMPOSITION over `GenericImage` + a typed handle
  wrapping `Container` (e.g. `PostgresContainer::connection_string()`), which already works on the
  current code and needs none of this split; `to_request()`/`run()` would only become an *option* for a
  module that needs run-level tweaks or polymorphic heterogeneity. (`src/GenericImage.cpp`; a future
  `ContainerRequest` in `include/testcontainers/` + `src/Runner.*`)

## Open / not yet built
- **Transport I/O timeouts** — the biggest robustness gap before production-grade; see the
  tech-debt item above.
- **Structured error hierarchy** — status/id fields + `TimeoutError` etc.; see the tech-debt
  item above.
- **`ContainerRequest` + `Runner` split of `start()`** — responsibility + testability; not a
  blocker. See the tech-debt item above.
- **`expose_host_ports` (Tier 2.8)** — deferred by decision (sshd sidecar + reverse tunnel via an
  optional libssh2 dependency). See the tech-debt item above.
- **Connection pooling / keep-alive** — `request()` opens one connection per call. See the
  "One connection per request" item above.
- **TLS end-to-end verification in CI** — the transport is implemented but unproven against a real
  remote TLS daemon. See the TLS item above.

> The earlier roadmap milestones (richer container config, `Mount`, RAII + Ryuk reaper, networks,
> mounts/volumes, copy-to/from, exec, Docker Compose) are all implemented — their current state and
> known limits live in the tech-debt list above.
