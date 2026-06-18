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
- **No streaming / follow logs** — `logs()` only does the non-follow snapshot;
  `LogOptions::follow` is sent but the body is fully buffered. A callback /
  incremental transport read is needed for the log-wait strategy.
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
  `src/WaitStrategies.cpp` (HTTP wait) re-implements `Container::get_host_port`'s IPv4-binding
  preference; factor into one shared helper. The HTTP wait also opens a fresh TCP connection +
  `io_context` per probe (fine for ~200ms polling).
- **msvc-preset configure noise** — under the Visual Studio (multi-config) preset, CMake prints
  non-fatal `IMPORTED_LOCATION ... _DEBUG ... Release` errors for OpenSSL/zlib because Conan
  installs Release-only; the Release build and tests still succeed. The default `ninja` preset is
  unaffected. (Install both configs, or filter the message, if it becomes annoying.)

## Next milestones
- Richer container config on `GenericImage` / `CreateContainerSpec`: entrypoint,
  working dir, user, privileged, mounts, networks, ulimits, host_config_modifier.
- `Mount` value type (bind / volume / tmpfs).

## Later
- Cleanup: RAII container + Ryuk reaper (crash-safe).
- Networks, mounts/volumes, copy-to/from (tar via libarchive), exec.
- Docker Compose support.
