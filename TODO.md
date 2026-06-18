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

## Next milestones (toward the redis MVP)
- Value types (`ContainerPort`, `Mount`, `WaitFor` variant, …) as plain copyable types.
- `GenericImage` / `Container` builder (in-place, ref-qualified `with_*`) + `start()` lifecycle.
- Wait strategies (log / http / healthcheck / exit) with the 60s startup timeout.
- Host-port discovery helper over inspect ports.
- **MVP:** `GenericImage("redis","7.2")` up → connect → auto-remove.

## Later
- Cleanup: RAII container + Ryuk reaper (crash-safe).
- Networks, mounts/volumes, copy-to/from (tar via libarchive), exec.
- Docker Compose support.
