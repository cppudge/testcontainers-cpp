# TODO / Backlog

Running list of known limitations, tech debt, and future work. Items found during
review are recorded here so they aren't lost between milestones.

## Known limitations / tech debt
- **`LogDemuxer` streaming cost** — `feed()` does `pending_.erase(0, pos)` on every
  call: O(n) per chunk → O(n²) for many small chunks. Fine for `demux_all` (single
  feed); switch to a consumed-offset / ring buffer when the follow/streaming path
  lands. (`src/docker/LogDemux.cpp`)
- **TTY containers not handled** — with `Tty=true` Docker returns a raw, unframed
  log stream; `demux_all` would garble it. We never enable TTY today, but add a raw
  path when TTY support is introduced. (`src/docker/LogDemux.cpp`)
- **follow logs: blocking + cooperative-stop only** — `DockerClient::follow_logs` /
  `Container::follow_logs` stream incrementally (`read_some` + `LogDemuxer`) and stop when
  the consumer returns false, but it is BLOCKING: run it on your own `std::thread` for
  background consumption. A background-thread RAII log handle with socket-level cancellation
  (stop even when no new bytes arrive) is not provided yet. (`src/docker/DockerClient.cpp`)
- **log-wait still polls snapshots** — the log wait in `src/WaitStrategies.cpp` re-fetches the
  full `tail=all` snapshot every 200ms; it could now be reimplemented on `follow_logs` (scan
  chunks, return false from the consumer once the substring count is reached).
- **TLS (https) transport not implemented** — `connect()` throws for the Https
  scheme. Needs `ssl::stream` + cert handling (`DOCKER_CERT_PATH`: ca/cert/key).
  (`src/docker/Transport.cpp`)
- **No registry auth** — `pull_image` sends no `X-Registry-Auth`; private images
  won't pull. (`src/docker/DockerClient.cpp`)
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
  (c) no `DOCKER_TLS_VERIFY` / `DOCKER_CERT_PATH` handling yet (that's the TLS-transport item).
  (`src/docker/DockerHost.cpp`, `src/docker/HostResolve.hpp`)
- **One connection per request** — `request()` opens/closes a transport each call
  (no keep-alive / pooling).
- **`get_host_port` IPv4/IPv6** — now prefers the IPv4 binding, but there are no
  explicit `get_host_port_ipv4/ipv6` accessors (cf. Rust's ipv4/ipv6 maps), and it
  re-inspects the container on every call (no caching of the published ports).
  (`src/Container.cpp`)
- **Log-wait polling cost** — the log wait re-fetches the full `tail=all` snapshot
  every 200ms; switch to an incremental follow-stream scan (ties to the follow-logs
  item above). (`src/WaitStrategies.cpp`)
- **Wait-strategy port resolution duplicated** — `mapped_host_port` in
  `src/WaitStrategies.cpp` (HTTP + port waits) re-implements `Container::get_host_port`'s IPv4-binding
  preference; factor into one shared helper. The HTTP/port waits also open a fresh TCP connection +
  `io_context` per probe (fine for ~200ms polling).
- **`wait::Port` probes only the externally mapped host port** — `wait_for::listening_port` resolves
  the published host port and does a TCP connect; it does NOT do the in-container `/proc/net/tcp`
  listening check that testcontainers-java additionally performs (`tcp_probe` in
  `src/WaitStrategies.cpp`). Adequate for "is the service reachable from the host", which is what tests
  need, but a container whose port is published before the process binds could read as ready early.
- **msvc-preset configure noise** — under the Visual Studio (multi-config) preset, CMake prints
  non-fatal `IMPORTED_LOCATION ... _DEBUG ... Release` errors for OpenSSL/zlib because Conan
  installs Release-only; the Release build and tests still succeed. The default `ninja` preset is
  unaffected. (Install both configs, or filter the message, if it becomes annoying.)
- **exec is buffered & unidirectional** — `Container::exec` reads the whole multiplexed output into
  memory and has no stdin/TTY (fine for run-command-capture-output); a streaming/interactive exec
  needs the hijacked-connection path.
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
- **Ryuk coverage & lifecycle** — only containers + networks get the session-id label, so future
  resource types (named volumes, images) must also be tagged to be reaped. The global `Reaper` has
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
- **build-from-Dockerfile: no .dockerignore, buffered output, unreaped images** —
  `ImageFromDockerfile::from_path` packs the whole directory tree (no `.dockerignore`
  filtering). `DockerClient::build_image` buffers the entire build-output stream (no live
  build-log streaming/consumer — could reuse the `follow_logs` chunked-read approach). Built
  images carry no Ryuk session-id label, so they are NOT auto-reaped (only containers/networks
  are); `with_no_cache`/`with_pull`/`with_target`/`with_build_arg` are supported, but secrets,
  ssh, cache-from, squash, and platform on build are not. (`src/ImageFromDockerfile.cpp`)
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
  `ImageFromDockerfile` / Compose / raw `DockerClient` calls are NOT substituted;
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

## Next milestones
- Richer container config on `GenericImage` / `CreateContainerSpec`: entrypoint,
  working dir, user, privileged, mounts, networks, ulimits, host_config_modifier.
- `Mount` value type (bind / volume / tmpfs).

## Later
- Cleanup: RAII container + Ryuk reaper (crash-safe).
- Networks, mounts/volumes, copy-to/from (tar via libarchive), exec.
- Docker Compose support.
