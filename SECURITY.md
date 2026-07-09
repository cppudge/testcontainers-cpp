# Security Policy

## Supported versions

testcontainers-cpp is pre-1.0. Only the latest release on the `0.1.x` line receives
security fixes; older tags do not.

| Version | Supported |
|---|---|
| 0.1.x (latest release) | yes |
| < latest 0.1.x | no |

## Reporting a vulnerability

Please report security issues **privately**, not as a public issue or pull request.

Use GitHub private vulnerability reporting: open the repository's **Security** tab and
choose **Report a vulnerability**
([direct link](https://github.com/cppudge/testcontainers-cpp/security/advisories/new)).
This creates a private advisory visible only to you and the maintainer.

Please include:

- the library version (or git commit) and your OS / compiler;
- how the Docker daemon is reached (Docker Desktop, native Engine, or a remote `tcp://` /
  `https://` endpoint) and the engine mode (Linux or Windows containers);
- a minimal reproducer or proof of concept, and the impact you observed.

## What to expect

This is a solo-maintained project, so timelines are best-effort:

- **Acknowledgement** within about a week of the report.
- An initial assessment (accepted / needs-info / out-of-scope) once the report is
  reproduced.
- For accepted issues, a fix or a documented mitigation in the supported `0.1.x` release,
  with the reporter credited in the advisory unless you prefer otherwise.

If you don't hear back within a week, please post a follow-up on the same advisory thread.

## Scope

testcontainers-cpp is a client that drives a **local or remote Docker daemon** over the
Docker Engine HTTP API. In scope:

- vulnerabilities in this library's own code — the HTTP transport (including the TLS path),
  registry-credential handling, tar/archive building for `copy_to`, request and command
  construction, and the log-demux framing.

Out of scope:

- vulnerabilities in Docker itself, the Docker daemon, or `containerd` — report those to
  their respective projects;
- vulnerabilities in the images or test containers you run (e.g. `redis`, `postgres`, the
  Ryuk reaper image, the sshd sidecar) — report those upstream;
- consequences of pointing the library at an **untrusted** Docker daemon or registry. The
  library trusts the daemon it is configured to talk to; a hostile daemon is outside the
  threat model.
