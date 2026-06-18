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
- **Docker host resolution is partial** — only `DOCKER_HOST` + platform default;
  the rootless socket fallbacks and `~/.testcontainers.properties` are not done
  (see `docs/01` §2).
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
  needs the hijacked-connection path. `Network` has no process-wide dedup (each `Network::create`
  makes a new network) and no inspect / connect-to-existing.
- **Ryuk coverage & lifecycle** — only containers + networks get the session-id label, so future
  resource types (named volumes, images) must also be tagged to be reaped. The global `Reaper` has
  no graceful in-process shutdown (relies on process-exit closing the socket); the Ryuk container is
  `AutoRemove`d on exit. Image pinned to `testcontainers/ryuk:0.11.0`.
- **Registry credential helpers unsupported** — `auth_from_docker_config` reads only plain `auths`
  (base64) + `identitytoken`; `credsStore`/`credHelpers` (the default on Docker Desktop) return
  nullopt (no subprocess to `docker-credential-*`). The Docker auth config is also re-read from disk
  on every pull (no caching). End-to-end private-registry pull isn't integration-tested (needs a
  reachable authenticated registry; flaky on Docker Desktop).
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
- **Windows containers: dotnet-parity only** — the engine mode is detected (`is_windows_engine()`) and
  Ryuk is skipped on the Windows engine, so there is **no crash-safe reaping** on Windows (RAII /
  AutoRemove only), matching testcontainers-dotnet. `copy-to-container` still Unix-normalizes the entry
  path, so `C:\...` targets aren't handled yet. Wait strategies are OS-agnostic (no PowerShell
  command-wait). The nanoserver test image tag is host-build-locked (`ltsc2025` on build 26100).
  A real Windows Ryuk (named-pipe mount + Windows reaper image) is unexplored — see `docs/04`.

## Next milestones
- Richer container config on `GenericImage` / `CreateContainerSpec`: entrypoint,
  working dir, user, privileged, mounts, networks, ulimits, host_config_modifier.
- `Mount` value type (bind / volume / tmpfs).

## Later
- Cleanup: RAII container + Ryuk reaper (crash-safe).
- Networks, mounts/volumes, copy-to/from (tar via libarchive), exec.
- Docker Compose support.
