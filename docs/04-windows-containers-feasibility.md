# Windows-container support — feasibility (based on testcontainers-dotnet)

> Research: how `testcontainers-dotnet` supports **Windows containers** (images that run an actual
> Windows OS, e.g. `mcr.microsoft.com/windows/nanoserver`), and what it would take in `testcontainers-cpp`.
> Sources cloned to `_research/testcontainers-dotnet` (gitignored).

## The surprising key finding

**testcontainers-dotnet does NOT run Ryuk for Windows containers — it detects the Windows engine and
*disables* the reaper.** (`ResourceReaper.GetAndStartDefaultAsync` → `if (isWindowsEngineEnabled) return null;`.)
There is no Windows Ryuk image and no named-pipe socket mount for it. Cleanup on Windows falls back to
each container's `AutoRemove` + explicit disposal (i.e. **no crash-safe reaping** on Windows).

So "Windows-container support" in dotnet is mostly three small things:
1. **Detect the Windows engine** — `GET /version`, check if the `Os` field contains "Windows".
2. **Skip Ryuk** when the Windows engine is detected.
3. Offer **opt-in PowerShell wait strategies** (`Wait.ForWindowsContainer()`) and pass a free-form
   `platform` string (e.g. `windows/amd64`) on create.

Crucially: the **Docker Engine API itself is identical** (same `/containers/create`, same named-pipe
transport). Almost nothing else changes — no `HostConfig.Isolation`, no per-OS create-body branching,
bind sources pass through verbatim.

## Delta mapping onto testcontainers-cpp

| Aspect | What dotnet does | Where we stand | Effort for us |
|---|---|---|---|
| **Transport** | same `npipe://./pipe/docker_engine` | ✅ we already use the named pipe | none |
| **Core endpoints** (create/start/inspect/logs/exec/...) | OS-agnostic, identical | ✅ work unchanged | none |
| **Daemon-OS detection** | `GET /version` → `Os` contains "Windows" | we already read `OSType` from `GET /_ping`; add a tiny `is_windows_engine()` | **trivial** |
| **Ryuk on Windows** | skipped entirely; rely on AutoRemove + RAII | gate `Reaper::ensure_started()` on `!is_windows_engine()`; we already have `auto_remove` + RAII | **trivial** |
| **`platform` field** | `CreateContainerParameters.Platform` → `?platform=` | not implemented | **small**: add `CreateContainerSpec.platform` + `?platform=` + `GenericImage::with_platform` |
| **Wait strategies** | log/http/health are OS-agnostic; cmd-waits use PowerShell (opt-in) | our 5 waits are log/duration/exit/healthcheck/http — **all OS-agnostic** (no shell exec) | none (our current set) |
| **exec / logs / host-port** | OS-agnostic | ✅ work unchanged | none |
| **copy-to-container paths** | always Unix-normalizes the in-container target (a latent bug for real Windows targets) | our tar strips the leading `/`; a `C:\...` target needs different handling | **small**: Windows-target path handling in the tar entry name |
| **Crash-safe cleanup on Windows** | **none** (Ryuk disabled) | same gap if we mirror dotnet | inherited limitation |

## Effort estimate

**Reaching parity with testcontainers-dotnet: LOW effort** — roughly a focused half-day:
- `is_windows_engine()` (one daemon query, cache it).
- Skip Ryuk on Windows in `Reaper::ensure_started()` (one conditional); optionally set `auto_remove=true`
  on user containers in that mode (matching dotnet) — with the caveat that AutoRemove only removes
  containers that *exit*, so it is **not** a crash-safety net for long-running services.
- Add `with_platform("windows/amd64")` + the `?platform=` query param.
- Make `CopyToContainer` targets Windows-aware (don't strip a `C:` drive; use the right separator/entry name).
- Most of our code (transport, endpoints, waits, exec, networks, host-port) already works because the
  API is the same and we're already on the named pipe.

**Going beyond dotnet — a real Windows Ryuk (crash-safe reaping on Windows): HIGH / RISKY.**
dotnet doesn't do it, so there's nothing to copy. It needs (a) a Windows-OS Ryuk image (the official
`testcontainers/ryuk` is Linux-only as referenced; a maintained Windows variant would have to be
sourced/verified) and (b) bind-mounting the named pipe `\\.\pipe\docker_engine` into a Windows Ryuk
container via the `npipe` mount type. This is net-new R&D with external dependencies; scope it
separately and only if crash-safe Windows cleanup is a hard requirement.

## Recommendation

Implement **dotnet parity** (the LOW-effort bullet list) as a small, self-contained phase: it's cheap,
unlocks running real Windows containers, and our architecture already lines up. Explicitly document the
**no-crash-safe-reaping-on-Windows** limitation (same as dotnet). Treat "Windows Ryuk" as a separate,
optional, exploratory follow-up rather than part of the initial support.

A prerequisite for testing: the user's Docker Desktop must be switched to "Windows containers" mode (or
run on Windows Server with the containers feature) — integration tests would `GTEST_SKIP` when the
daemon is in Linux mode.

## Testing / engine modes (as implemented)

The dotnet-parity slice is now implemented. Because Docker Desktop runs **either** Linux **or** Windows
containers (never both) and the daemon only runs images matching the current mode, the integration
suite is **engine-aware** rather than mode-locked:

- `tests/integration/EngineGuard.hpp` (namespace `tcit`) exposes `linux_engine_unavailable()` and
  `windows_engine_unavailable()`. Each returns a skip *reason* (or `std::nullopt`) — it does **not**
  call `GTEST_SKIP` itself, so it is safe to call from a fixture's `SetUp()`:
  `if (auto why = tcit::linux_engine_unavailable()) GTEST_SKIP() << *why;`.
- "Unavailable" means the daemon is unreachable **or** the engine is in the wrong mode (Windows for the
  Linux-image tests, Linux for the `WindowsContainer` suite). Engine detection uses
  `DockerClient::is_windows_engine()` (`GET /version` → `Os`, cached process-wide).
- Net effect: `ctest` runs the suite matching whatever mode the host is in. In **Windows-containers**
  mode all Linux-image tests skip (with a clear reason) and the `WindowsContainer` suite runs a real
  `mcr.microsoft.com/windows/nanoserver` container (echo+logs, and exec into a ping keep-alive). In
  **Linux** mode the inverse holds and `WindowsContainer` skips. The unit suite is daemon-free and
  always runs (including `parse_server_os` and the `platform` create-query mapping).
- Image tag note: nanoserver tags are host-OS-version-locked. On Windows build 26100, `ltsc2025` runs;
  the older `ltsc2022` fails with "container operating system does not match host". Pick the tag that
  matches the host build (the `WindowsContainer` test pins `ltsc2025`).
