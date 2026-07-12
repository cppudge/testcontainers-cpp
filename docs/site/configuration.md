# Configuration

With Docker Desktop or a local daemon **no configuration is needed**. Everything on this page
is for the other topologies: remote daemons, TLS, CI containers, registry mirrors.

## How the daemon endpoint is resolved

The standard Testcontainers order, first hit wins:

1. `DOCKER_HOST` environment variable
2. `docker.host` in `~/.testcontainers.properties`
3. The active **docker context** (`DOCKER_CONTEXT`, else the config's `currentContext`) —
   including its TLS certificate store
4. Platform default — `unix:///var/run/docker.sock` (with rootless-socket fallbacks) on
   Linux/macOS, the `//./pipe/docker_engine` named pipe on Windows

`ssh://` endpoints (which a docker context can carry) are not supported and fall through to
the platform default.

## The properties file

Every library switch reads an **environment variable first**, then a key of
`~/.testcontainers.properties` (under `HOME`, else `USERPROFILE`; read once per process).
A *set* env var decides even when it decides "off" — an explicit `TESTCONTAINERS_RYUK_DISABLED=false`
overrides a file-enabled switch.

The file is shared with the other Testcontainers implementations, so parsing matches
testcontainers-java where they could diverge: `#`/`!` comment lines, last duplicate key wins,
booleans follow `Boolean.parseBoolean` (case-insensitive `true`; `1` is **false**) — except
`docker.tls.verify`, where `1` also counts as true (docker-java parity).

| Environment variable | Properties key | Meaning |
|---|---|---|
| `DOCKER_HOST` | `docker.host` | daemon endpoint (`unix://`, `npipe://`, `tcp://`, `https://`) |
| `DOCKER_TLS_VERIFY` | `docker.tls.verify` | upgrade a `tcp://` endpoint to verified TLS (port 2375 → 2376) |
| `DOCKER_CERT_PATH` | `docker.cert.path` | directory with `ca.pem` / `cert.pem` / `key.pem` |
| `TESTCONTAINERS_HOST_OVERRIDE` | `host.override` | address handed out for reaching published ports |
| `TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX` | `hub.image.name.prefix` | registry mirror prefix for Docker-Hub image references |
| `TESTCONTAINERS_RYUK_DISABLED` | `ryuk.disabled` | disable the [Ryuk reaper](core-concepts.md#cleanup-raii-ryuk) |
| `TESTCONTAINERS_RYUK_CONTAINER_IMAGE` | `ryuk.container.image` | override the reaper image (`testcontainers/ryuk:0.11.0`) |
| `TESTCONTAINERS_SSHD_CONTAINER_IMAGE` | `sshd.container.image` | override the host-access sidecar image |
| `TESTCONTAINERS_SOCAT_CONTAINER_IMAGE` | `socat.container.image` | override the compose ambassador image |
| `TESTCONTAINERS_COMPOSE_CONTAINER_IMAGE` | `compose.container.image` | override the containerised compose client image |
| `TESTCONTAINERS_REUSE_ENABLE` | `testcontainers.reuse.enable` | globally enable [container reuse](core-concepts.md#cleanup-raii-ryuk) |

(The mapping rule: env name = `TESTCONTAINERS_` + the key upper-cased with dots as
underscores — with two exceptions: the `docker.*` trio keeps its standard `DOCKER_*` names,
and a key already starting with `testcontainers.` is not doubled, hence
`TESTCONTAINERS_REUSE_ENABLE`.)

## TLS and remote daemons

- An explicit `https://host:port` endpoint dials TLS directly.
- The docker-CLI spelling — `tcp://host:port` **plus** `DOCKER_TLS_VERIFY` /
  `docker.tls.verify` — upgrades to TLS, moving the default port 2375 → 2376. Client
  certificates come from `DOCKER_CERT_PATH` / `docker.cert.path`, falling back to `~/.docker`.
- A **docker context** carries its own TLS store; those materials are used verbatim, and the
  context's `SkipTLSVerify` turns server verification off while still presenting the client
  pair.
- Mutual TLS is exercised end to end in CI against a real `--tlsverify` docker:dind daemon.

One deliberate divergence from the docker CLI: the CLI treats *any* non-empty
`DOCKER_TLS_VERIFY` as "verify" (even `0`); this library recognizes `1`/`true` only.

The TLS transport is a build option (`TC_TLS`, default ON) — with it off, an `https://`
endpoint throws a `DockerError` naming the option.

## Running inside a container (Docker-in-Docker / CI)

With the daemon socket mounted into your CI container, set **nothing**: the library detects
the situation (`/.dockerenv`) and hands out the docker bridge gateway instead of `localhost`,
so published ports resolve correctly. With a remote agent in between (e.g. a cloud runner
architecture), set `TESTCONTAINERS_HOST_OVERRIDE` to wherever published ports actually
surface.

## Registry authentication

Credentials for pulls resolve the standard way — first hit wins:

1. Explicit `with_registry_auth(...)` on the image
2. `DOCKER_AUTH_CONFIG` (the JSON itself, CI-friendly)
3. `$DOCKER_CONFIG/config.json`, else `~/.docker/config.json` — including **credential
   helpers** (`credsStore` / `credHelpers`, shelling out to `docker-credential-<helper> get`);
   plaintext `auths` entries take precedence

Helper outcomes are cached per (helper, registry) for 5 minutes — including "no credentials",
the answer for every anonymous pull under Docker Desktop's `credsStore`.

!!! warning "404 can mean 'authentication required'"
    Registries answer 404 for private images requested without credentials, so a pull
    `NotFoundError` on an image you know exists usually means the credentials didn't resolve.

## Image name substitution

`TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX` (or a custom `with_image_name_substitutor`) redirects
**Docker-Hub** references to your mirror — including every internal utility image the library
starts (Ryuk, the sshd sidecar, the socat ambassador, the containerised compose client,
volume-populate helpers). Registry-qualified references pass through untouched. Not
substituted by design: images built by `GenericBuildableImage` (the daemon resolves `FROM`),
services in compose YAML (the file is yours), and raw `DockerClient` calls.
