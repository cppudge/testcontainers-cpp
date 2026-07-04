# Public API integration-test coverage

Audit date: 2026-07-05.

This file audits the integration-test coverage of every public interface under
`include/testcontainers/` against a real Docker daemon, in each of the two engine
modes. It is a companion to [06-feature-notes.md](06-feature-notes.md) (what
exists and its limits); this one asks the narrower question: *is each public
function exercised by a test in `tests/integration/` against a live daemon, and
in which engine mode?*

Engine modes, as run on CI:

- **Linux engine** — the ubuntu CI job, a Linux-containers daemon. Fixtures skip
  themselves when the daemon is in Windows mode (`tcit::linux_engine_unavailable`).
- **Windows engine** — the windows-2022 CI job, a Windows-containers daemon
  (process isolation, nanoserver base). Fixtures skip on any other engine
  (`tcit::WindowsEngineTest` / `tcit::windows_engine_unavailable`).

Column meaning:

- **Works on Linux / Windows engine** — does the feature function against that
  daemon mode, from the code, the feature notes, and the docs. Daemon-side
  engine constraints (e.g. `Isolation` is Windows-only; `Ulimits` / `CapAdd` /
  tmpfs are Linux-only) are reflected here.
- **Integration-tested (Linux / Windows)** — exercised by a `tests/integration/`
  test against a real daemon in that mode; the cell names the `Suite.TestName`.
  Unit-only coverage does **not** count (noted as "unit-tested" where relevant).

Legend: ✅ yes · ❌ no · n/a not applicable in that mode · ? undetermined.

---

## GenericImage (builder + `start()`)

`GenericImage` is the primary entry point; `start()` is `run(to_request())`.
`start()` and the returned handle are the most heavily exercised surface in the
suite.

| Function | Works on Linux | Works on Windows | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|---|---|
| `GenericImage(image, tag)` | ✅ | ✅ | ✅ many (RedisMvp, WaitStrategies, Exec, …) | ✅ via `nanoserver()` (WindowsContainer.*, etc.) |
| `from_reference(ref)` | ✅ | ✅ | ✅ PortGetters.*, Volumes.PopulateThenReadBack, Lifecycle.* | ❌ |
| `start()` | ✅ | ✅ | ✅ RedisMvp.StartsConnectsAndAutoRemoves (+ most suites) | ✅ WindowsContainer.EchoExitsWithExpectedLogs |
| `to_request()` | ✅ | ✅ | ❌ (unit-tested; every `start()` uses it internally) | ❌ |
| `with_exposed_port` | ✅ | ✅ [a] | ✅ RedisMvp, PortGetters.*, WaitStrategies.* | ❌ [a] |
| `with_env` | ✅ | ✅ | ❌ [b] | ❌ [b] |
| `with_cmd` | ✅ | ✅ | ✅ nearly every Linux suite | ✅ WindowsContainer.EchoExitsWithExpectedLogs |
| `with_entrypoint` | ✅ | ✅ | ✅ ContainerConfig.EntrypointOverride | ❌ |
| `with_working_dir` | ✅ | ✅ | ✅ ContainerConfig.WorkingDirAndUser | ❌ [c] |
| `with_user` | ✅ | ✅ | ✅ ContainerConfig.WorkingDirAndUser | ✅ WindowsVolumes.DataPersistsAcrossContainers |
| `with_privileged` | ✅ | ❌ (not supported) | ❌ | n/a |
| `with_isolation` | ❌ (daemon rejects non-default) | ✅ | n/a | ✅ implicit — `nanoserver()` sets `with_isolation("process")` for every Windows test |
| `with_tty` | ✅ | ✅ | ✅ Tty.LogsAreRawNotFramed, Tty.FollowLogsDeliversRaw | ❌ [d] |
| `with_mount` (bind) | ✅ | ✅ | ❌ | ❌ |
| `with_mount` (volume) | ✅ | ✅ | ✅ Volumes.PopulateThenReadBack | ✅ WindowsVolumes.DataPersistsAcrossContainers |
| `with_mount` (tmpfs) | ✅ | ❌ (Linux-only) | ✅ ContainerConfig.TmpfsMount | n/a |
| `with_copy_to` | ✅ | ✅ | ✅ Copy.CopyAtStartData, Copy.CopyAtStartHostFile | ✅ WindowsCopy.CopyAtStartData, WindowsCopy.CopyAtStartHostFile |
| `with_label` | ✅ | ✅ | ❌ [e] | ❌ |
| `with_wait` | ✅ | ✅ | ✅ WaitStrategies.* (+ most suites) | ✅ WindowsContainer.EchoExitsWithExpectedLogs (exit wait) |
| `with_startup_timeout` | ✅ | ✅ | ✅ WaitStrategies.TimeoutThrowsStartupTimeoutError, Lifecycle.StartupRetriesOnFailure | ❌ |
| `with_healthcheck` | ✅ | ✅ | ✅ WaitStrategies.HealthcheckWaitBecomesHealthy | ❌ |
| `with_network` | ✅ | ✅ | ✅ Networks.ResolvesPeerByContainerName | ✅ WindowsNetworks.PeerNameRegisteredAndReachable |
| `with_network_alias` | ✅ | ✅ | ✅ Networks.AliasResolvesOnCustomNetwork | ✅ WindowsNetworks.AliasRegisteredOnCustomNetwork |
| `with_container_name` | ✅ | ✅ | ✅ Networks.ResolvesPeerByContainerName | ✅ WindowsNetworks.PeerNameRegisteredAndReachable |
| `with_platform` | ✅ | ✅ | ❌ | ❌ |
| `with_registry_auth` | ✅ | ✅ | ❌ [f] | ❌ |
| `with_memory_limit` | ✅ | ✅ | ❌ | ❌ |
| `with_shm_size` | ✅ | ❌ (Linux-only) | ❌ | n/a |
| `with_ulimit` | ✅ | ❌ (Linux-only) | ✅ ContainerConfig.UlimitApplied | n/a |
| `with_cap_add` | ✅ | ❌ (Linux-only) | ❌ | n/a |
| `with_cap_drop` | ✅ | ❌ (Linux-only) | ❌ | n/a |
| `with_extra_host` | ✅ | ✅ | ✅ ContainerConfig.ExtraHostApplied | ❌ |
| `with_exposed_host_port` | ✅ | ❌ (throws; sshd sidecar is Linux) | ✅ HostAccess.* | n/a |
| `with_create_body_patch` | ✅ | ✅ | ❌ | ❌ |
| `with_image_pull_policy` | ✅ | ✅ | ✅ ContainerConfig.AlwaysPullPolicyStarts | ❌ |
| `with_reuse` | ✅ | ✅ | ✅ Reuse.ReuseAdoptsRunningContainer, Reuse.ReuseDisabledCreatesFresh | ❌ |
| `with_image_name_substitutor` | ✅ | ✅ | ✅ ContainerConfig.CustomSubstitutorRewritesImage | ❌ |
| `with_created_hook` | ✅ | ✅ | ✅ Lifecycle.HooksFireInOrder | ❌ |
| `with_starting_hook` | ✅ | ✅ | ✅ Lifecycle.HooksFireInOrder | ❌ |
| `with_started_hook` | ✅ | ✅ | ✅ Lifecycle.HooksFireInOrder | ❌ |
| `with_stopping_hook` | ✅ | ✅ | ✅ Lifecycle.StoppingHookFiresOnStop | ❌ |
| `with_startup_attempts` | ✅ | ✅ | ✅ Lifecycle.StartupRetriesOnFailure | ❌ |
| getters (`image()`, `env()`, …) | ✅ | ✅ | unit-tested | unit-tested |

Notes:
- [a] Port publishing works on the Windows daemon (nat driver), but nanoserver
  ships no server binary to listen on a port, so there is no Windows-mode test
  for a published/mapped exposed port. See "Gaps worth closing".
- [b] `with_env` is passed on every start and is used as a reuse-hash marker in
  the Reuse suite, but no test asserts the variable is visible *inside* the
  container. (Exec.PassesEnv covers `ExecOptions.env`, a different path.)
- [c] Container-level `WorkingDir` on Windows is untested; WindowsExec.UsesWorkingDir
  covers the exec-level working dir instead.
- [d] Container-level `Tty=true` on Windows is untested; WindowsExec.TtyCapturesRawStdout
  covers the exec-level TTY path.
- [e] User labels are never asserted; ReaperTest asserts the *session* labels
  the runner injects, not `with_label` values.
- [f] `with_registry_auth` is untested at the `GenericImage` level. AuthTest
  exercises the credential path through `DockerClient::pull_image` instead.

---

## GenericBuildableImage (build from Dockerfile)

| Function | Works on Linux | Works on Windows | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|---|---|
| `GenericBuildableImage(name, tag)` | ✅ | ✅ | ✅ BuildImage.* | ✅ WindowsBuildImage.* |
| `with_dockerfile(path)` | ✅ | ✅ | ❌ | ❌ |
| `with_dockerfile_string` | ✅ | ✅ | ✅ BuildImage.BuildsAndRunsInlineDockerfile | ✅ WindowsBuildImage.BuildsAndRunsInlineDockerfile |
| `with_file(path, target)` | ✅ | ✅ | ❌ | ❌ |
| `with_data(bytes, target)` | ✅ | ✅ | ❌ | ❌ |
| `with_build_arg` | ✅ | ✅ | ❌ | ❌ |
| `with_target` | ✅ | ✅ | ❌ | ❌ |
| `with_no_cache` | ✅ | ✅ | ❌ | ❌ |
| `with_pull` | ✅ | ✅ | ❌ | ❌ |
| `build()` | ✅ | ✅ | ✅ BuildImage.BuildsAndRunsInlineDockerfile, BuildImage.BuildFailureThrows | ✅ WindowsBuildImage.BuildsAndRunsInlineDockerfile, WindowsBuildImage.BuildFailureThrows |
| `descriptor()`, getters | ✅ | ✅ | unit-tested | unit-tested |

Only the inline-Dockerfile + build-error round-trip is covered in both modes.
Host-file/dir context (`with_dockerfile(path)`, `with_file`, `with_data`) and
the build knobs (`with_build_arg`, `with_target`, `with_no_cache`, `with_pull`)
have no integration coverage in either mode.

---

## Container (RAII handle)

| Function | Works on Linux | Works on Windows | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|---|---|
| `adopt(client, id, ownership, tty)` | ✅ | ✅ | ❌ | ❌ |
| `id()` | ✅ | ✅ | ✅ RedisMvp, Reuse, Networks | ✅ WindowsNetworks.* (`srv.id()`) |
| `is_persistent()` | ✅ | ✅ | ✅ Reuse.ReuseAdoptsRunningContainer | ❌ |
| `has_tty()` | ✅ | ✅ | ❌ (unit path via logs) | ❌ |
| `host()` | ✅ | ✅ | ✅ RedisMvp.StartsConnectsAndAutoRemoves | ❌ |
| `get_host_port` | ✅ | ✅ [a] | ✅ RedisMvp, WaitStrategies.*, PortGetters.* | ❌ [a] |
| `get_host_port_ipv4` | ✅ | ✅ [a] | ✅ PortGetters.Ipv4AndDefaultAgree | ❌ [a] |
| `get_host_port_ipv6` | ? (daemon-dependent) | ? | ✅ PortGetters.Ipv4AndDefaultAgree (tolerant: resolves or throws) | ❌ |
| `first_mapped_port` | ✅ | ✅ [a] | ✅ PortGetters.FirstMappedPicksExposedOrder | ❌ [a] |
| `inspect()` | ✅ | ✅ | ✅ PortGetters.InspectAndRaw | ❌ [b] |
| `inspect_raw()` | ✅ | ✅ | ✅ PortGetters.InspectAndRaw | ❌ [b] |
| `logs()` | ✅ | ✅ | ✅ ContainerConfig.*, Tty.LogsAreRawNotFramed | ✅ WindowsContainer.EchoExitsWithExpectedLogs, WindowsBuildImage.* |
| `follow_logs()` | ✅ | ✅ | ✅ Tty.FollowLogsDeliversRaw | ❌ |
| `exec(cmd)` | ✅ | ✅ | ✅ Exec.CapturesStdoutAndZeroExit | ✅ WindowsContainer.ExecRunsInRunningContainer |
| `exec(cmd, opts)` | ✅ | ✅ | ✅ Exec.PassesEnv/UsesWorkingDir/RunsAsUser/TtyCapturesRawStdout/FeedsStdin | ✅ WindowsExec.* |
| `exec(cmd, opts, consumer)` | ✅ | ✅ | ✅ Exec.StreamsOutputIncrementally, Exec.StreamingStopsWhenConsumerReturnsFalse | ✅ WindowsExec.StreamsOutputIncrementally, WindowsExec.StreamingStopsWhenConsumerReturnsFalse |
| `copy_to(source)` | ✅ | ✅ [c] | ✅ Copy.CopyIntoRunningContainer | ✅ WindowsCopy.CopyIntoRunningContainer |
| `read_file(path)` | ✅ | ✅ [c] | ✅ Copy.ReadFileRoundTrip, Copy.LargeFileRoundTrip, Copy.ReadFileRejectsDirectory | ✅ WindowsCopy.ReadFileRoundTrip, WindowsCopy.LargeFileRoundTrip, WindowsCopy.ReadFileRejectsDirectory |
| `copy_file_from(path, host)` | ✅ | ✅ [c] | ✅ Copy.CopyFileFromWritesHost | ✅ WindowsCopy.CopyFileFromWritesHost |
| `stop()` | ✅ | ✅ | ✅ Lifecycle.StoppingHookFiresOnStop | ❌ |
| `is_running()` | ✅ | ✅ | ✅ RedisMvp, WaitStrategies.* | ✅ WindowsContainer.ExecRunsInRunningContainer |
| `remove()` | ✅ | ✅ | ✅ implicit via RAII drop everywhere | ✅ implicit via RAII drop |

Notes:
- [a] The port getters function on the Windows daemon but are untested there —
  no Windows test image publishes a listening port.
- [b] `is_running()` (used in WindowsContainer/WindowsExec) inspects internally,
  so the inspect path is indirectly exercised on Windows; `inspect()` /
  `inspect_raw()` themselves are not called directly in a Windows test.
- [c] Windows filesystem ops require **process** isolation; Docker Desktop's
  default Hyper-V isolation rejects copy/read against a running container. The
  Windows tests pin process isolation via `nanoserver()`.

---

## Network (RAII handle + Builder)

| Function | Works on Linux | Works on Windows | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|---|---|
| `Network::create(name)` | ✅ | ✅ | ✅ Networks.CreateAndRemove | ✅ WindowsNetworks.CreateAndRemove |
| `Network::create()` | ✅ | ✅ | ✅ Networks.ResolvesPeerByContainerName, Networks.AliasResolvesOnCustomNetwork | ✅ WindowsNetworks.PeerNameRegisteredAndReachable, WindowsNetworks.AliasRegisteredOnCustomNetwork |
| `name()` / `id()` | ✅ | ✅ | ✅ Networks.CreateAndRemove | ✅ WindowsNetworks.CreateAndRemove |
| `remove()` (+ idempotent) | ✅ | ✅ | ✅ Networks.CreateAndRemove | ✅ WindowsNetworks.CreateAndRemove |
| `connect(id, aliases)` | ✅ | ✅ | ❌ [a] | ❌ [a] |
| `builder()` + `create()` | ✅ | ✅ | ✅ Networks.BuilderCreatesNetwork | ✅ WindowsNetworks.BuilderCreatesNetwork |
| `Builder::with_driver` | ✅ | ✅ | ✅ Networks.BuilderCreatesNetwork ("bridge") | ✅ WindowsNetworks.BuilderCreatesNetwork ("nat") |
| `Builder::with_attachable` | ✅ | ❌ (HNS rejects) | ✅ Networks.BuilderCreatesNetwork | n/a |
| `Builder::with_subnet` | ✅ | ✅ | ✅ Networks.BuilderCreatesNetwork | ✅ WindowsNetworks.BuilderCreatesNetwork |
| `Builder::with_name` | ✅ | ✅ | ❌ | ❌ |
| `Builder::with_internal` | ✅ | ✅ | ❌ | ❌ |
| `Builder::with_enable_ipv6` | ✅ | ? | ❌ | ❌ |
| `Builder::with_gateway` | ✅ | ✅ | ❌ | ❌ |
| `Builder::with_option` | ✅ | ✅ | ❌ | ❌ |
| `Builder::with_label` | ✅ | ✅ | ❌ | ❌ |

Notes:
- [a] `Network::connect` (attach an *already-running* container) has no test.
  Every network test attaches at create time via `GenericImage::with_network`,
  which routes through `create_container`, not `connect_network`.

---

## Volume (RAII handle + Builder)

| Function | Works on Linux | Works on Windows | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|---|---|
| `Volume::create(name)` | ✅ | ✅ | ✅ Networks/Volumes via name path | ❌ (only `create()` used) |
| `Volume::create()` | ✅ | ✅ | ✅ Volumes.CreateInspectRemove, Volumes.RaiiRemovesOnDrop, Volumes.PopulateThenReadBack | ✅ WindowsVolumes.CreateInspectRemove, WindowsVolumes.DataPersistsAcrossContainers |
| `name()` | ✅ | ✅ | ✅ Volumes.CreateInspectRemove | ✅ WindowsVolumes.CreateInspectRemove |
| `remove()` | ✅ | ✅ | ✅ Volumes.CreateInspectRemove, Volumes.RaiiRemovesOnDrop | ✅ WindowsVolumes.CreateInspectRemove |
| `inspect()` | ✅ | ✅ | ✅ Volumes.CreateInspectRemove | ✅ WindowsVolumes.CreateInspectRemove |
| `populate(sources, …)` | ✅ | ❌ (Linux-only; archive upload lands in the layer, bypassing the mount) | ✅ Volumes.PopulateThenReadBack | n/a [a] |
| `builder()` + `create()` | ✅ | ✅ | ❌ | ❌ |
| `Builder::with_driver` / `with_driver_opt` / `with_label` / `with_name` | ✅ | ✅ | ❌ | ❌ |

Notes:
- [a] WindowsVolumes.DataPersistsAcrossContainers substitutes for populate() on
  Windows: it seeds the volume by writing from inside a mounted container
  (`exec`), the mechanism Windows users must use.

---

## DockerComposeContainer

Compose requires Linux images and a compose CLI/ambassador in practice; there is
no Windows-mode compose test (nor a realistic Windows compose stack here). Every
row's Windows column is n/a for that reason.

| Function | Works on Linux | Works on Windows | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|---|---|
| ctor(files) / ctor(file) | ✅ | n/a | ❌ (from_yaml used instead) | n/a |
| `with_local_client` / `with_containerised_client` / `with_auto_client` | ✅ | n/a | ❌ (factory form; `with_client` used) | n/a |
| `from_yaml(yaml)` | ✅ | n/a | ✅ Compose.LocalClientBringsUpRedis (+ all) | n/a |
| `with_client(kind)` | ✅ | n/a | ✅ Compose.ContainerisedClientBringsUpRedis (Containerised), Compose.AutoClientBringsUpRedis (Auto), Compose.LocalClientBringsUpRedis (Local default) | n/a |
| `with_exposed_service` | ✅ | n/a | ✅ Compose.* | n/a |
| `with_project_name` | ✅ | n/a | ❌ | n/a |
| `with_compose_image` | ✅ | n/a | ❌ (default docker:26.1-cli used) | n/a |
| `with_env` / `with_env_vars` | ✅ | n/a | ❌ | n/a |
| `with_build` | ✅ | n/a | ❌ | n/a |
| `with_pull` | ✅ | n/a | ❌ | n/a |
| `with_wait` | ✅ | n/a | ❌ (default on; not toggled) | n/a |
| `with_wait_timeout` | ✅ | n/a | ❌ | n/a |
| `with_remove_volumes` / `with_remove_images` | ✅ | n/a | ❌ | n/a |
| `start()` | ✅ | n/a | ✅ Compose.* (+ restart: RestartKeepsProjectAlive) | n/a |
| `stop()` | ✅ | n/a | ✅ Compose.* (+ label sweep assertion) | n/a |
| `get_service_host` | ✅ | n/a | ✅ Compose.* | n/a |
| `get_service_port` | ✅ | n/a | ✅ Compose.* | n/a |
| `get_service_container_id` | ✅ | n/a | ✅ Compose.RestartKeepsProjectAlive | n/a |
| `project_name()` | ✅ | n/a | ✅ Compose.* | n/a |
| getters (`compose_files`, `client_kind`, …) | ✅ | n/a | unit-tested | n/a |

Coverage is the redis PING/PONG round-trip across all three client kinds
(Local/Containerised/Auto) plus a restart. The many configuration setters
(`with_env`, `with_build`, `with_pull`, `with_project_name`,
`with_compose_image`, `with_wait_timeout`, teardown flags) are not integration-
covered.

---

## Wait strategies (`WaitFor` / `wait_for::*`)

`WaitFor` is a value type; coverage means a container was actually gated on that
strategy. Windows integration coverage is limited to the exit strategy — see the
port note below.

| Strategy | Works on Linux | Works on Windows | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|---|---|
| `wait::None` (no wait) | ✅ | ✅ | ✅ implicit (Exec/Copy/Network containers start with no wait) | ✅ implicit (WindowsExec keep-alive containers) |
| `stdout_message` / `stderr_message` / `log` | ✅ | ✅ | ✅ RedisMvp (stdout), Tty.LogWaitWorksOnTtyContainer, WaitStrategies.TimeoutThrows (log) | ❌ |
| `seconds` / `millis` (Duration) | ✅ | ✅ | ❌ | ❌ |
| `exit` / `exit_code` | ✅ | ✅ | ✅ WaitStrategies.ExitCodeWaitSucceeds, BuildImage.* | ✅ WindowsContainer.EchoExitsWithExpectedLogs, WindowsBuildImage.* |
| `healthy` (Healthcheck) | ✅ | ✅ | ✅ WaitStrategies.HealthcheckWaitBecomesHealthy | ❌ |
| `http` | ✅ | ✅ [a] | ✅ WaitStrategies.HttpWaitReachesNginx | ❌ [a] |
| `listening_port` (Port) | ✅ | ✅ [a] | ✅ WaitStrategies.PortWaitReachesRedis | ❌ [a] |

Notes:
- [a] The HTTP and listening-port strategies work against a Windows daemon in
  principle, but nanoserver ships no listening server, so no Windows test
  exercises them (feature-notes / TODO record this gap).

---

## Healthcheck / Mount / CopyToContainer / ExecOptions / ExecResult (value types)

These are copyable value types; their behavior is verified through the modules
that consume them (rows above). Summary of where each is exercised:

| Type | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|
| `Healthcheck` (cmd_shell, interval/retries/start_period) | ✅ WaitStrategies.HealthcheckWaitBecomesHealthy | ❌ |
| `Healthcheck::cmd` / `::none` | ❌ (unit-tested) | ❌ |
| `Mount::bind` | ❌ | ❌ |
| `Mount::volume` | ✅ Volumes.PopulateThenReadBack | ✅ WindowsVolumes.DataPersistsAcrossContainers |
| `Mount::tmpfs` (+ size/mode) | ✅ ContainerConfig.TmpfsMount | n/a (Linux-only) |
| `CopyToContainer::content` | ✅ Copy.CopyAtStartData | ✅ WindowsCopy.CopyAtStartData |
| `CopyToContainer::host_file` | ✅ Copy.CopyAtStartHostFile | ✅ WindowsCopy.CopyAtStartHostFile |
| `CopyToContainer::with_mode` | ❌ (unit-tested) | ❌ |
| `ExecOptions` (env/working_dir/user/tty/stdin_data) | ✅ Exec.* | ✅ WindowsExec.* |
| `ExecOptions.privileged` | ❌ | ❌ |
| `ExecResult` (stdout/stderr/exit_code) | ✅ Exec.* | ✅ WindowsExec.* |

`Mount::bind` (bind mounts) has no integration coverage in either engine.
`ExecOptions.privileged` and `CopyToContainer::with_mode` are likewise
integration-uncovered.

---

## Lifecycle hooks

Covered under GenericImage above (Lifecycle.HooksFireInOrder,
Lifecycle.StoppingHookFiresOnStop, Lifecycle.StartupRetriesOnFailure) — Linux
only. The `LifecycleHook` typedef itself has no separate surface. No Windows-mode
hook test exists.

---

## DockerClient (public low-level client)

The engine-detection and transport-shape methods are engine-agnostic; the CRUD
methods are mostly driven directly on Linux and indirectly (through GenericImage /
Network / Volume) on Windows.

| Function | Works on Linux | Works on Windows | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|---|---|
| `DockerClient(host)` / `from_environment()` | ✅ | ✅ | ✅ everywhere | ✅ WindowsEngine fixtures |
| `Session` (keep-alive reuse) | ✅ | ✅ | ✅ implicit (wait-strategy polling) | ❌ |
| `host()` | ✅ | ✅ | ✅ RedisMvp, Exec (scheme) | ❌ |
| `set/transport_timeouts` | ✅ | ✅ | ❌ (unit-tested) | ❌ |
| `request(method, target, …)` | ✅ | ✅ | ✅ ReaperTest, NetworkTest (raw inspect) | ✅ WindowsEngine `/version`, WindowsNetworks raw inspect |
| `ping()` | ✅ | ✅ | ✅ EngineGuard (every suite's SetUp) | ✅ EngineGuard |
| `server_os()` | ✅ | ✅ | ✅ WindowsEngine tag resolution | ✅ WindowsEngine (implicitly, via is_windows_engine) |
| `is_windows_engine()` | ✅ | ✅ | ✅ EngineGuard | ✅ EngineGuard |
| `pull_image(image, auth?)` | ✅ | ✅ | ✅ DockerLifecycle, AuthTest, DockerLogs | ❌ (pull happens via create in Windows tests) |
| `build_image(tar, opts)` | ✅ | ✅ | ✅ via GenericBuildableImage (BuildImage.*) | ✅ via GenericBuildableImage (WindowsBuildImage.*) |
| `create_container(spec, auth?)` | ✅ | ✅ | ✅ DockerLifecycle.*, ReaperTest, DockerLogs | ❌ (Windows tests go through `start()`) |
| `start_container(id)` | ✅ | ✅ | ✅ DockerLifecycle.CreateStartInspectRemove | ❌ |
| `inspect_container(id)` | ✅ | ✅ | ✅ DockerLifecycle.*, Compose.RestartKeepsProjectAlive | ❌ (indirect via is_running) |
| `inspect_container_raw(id)` | ✅ | ✅ | ✅ via Container.inspect_raw (PortGetters) | ❌ |
| `list_containers(filters, all)` | ✅ | ✅ | ✅ Compose.* (project-label sweep) | ❌ |
| `stop_container(id, timeout?)` | ✅ | ✅ | ✅ DockerLifecycle.CreateStartInspectRemove | ❌ |
| `remove_container(id, force, vols)` | ✅ | ✅ | ✅ RemoveGuard, Reuse cleanup | ❌ (indirect via RAII) |
| `logs(id, opts)` | ✅ | ✅ | ✅ DockerLogs.FetchesStdoutAndStderr | ❌ (Container.logs used instead) |
| `follow_logs(id, opts, consumer)` | ✅ | ✅ | ✅ DockerLogs.FollowStreamsUntilExit, DockerLogs.FollowStopsEarly… | ❌ |
| `exec(id, cmd[, opts[, consumer]])` | ✅ | ✅ | ✅ via Container (Exec.*) | ✅ via Container (WindowsExec.*) |
| `copy_to_container(id, source)` | ✅ | ✅ | ✅ via Container.copy_to (Copy.*) | ✅ via Container.copy_to (WindowsCopy.*) |
| `copy_from_container(id, path)` | ✅ | ✅ | ✅ via Container.read_file (Copy.*) | ✅ via Container.read_file (WindowsCopy.*) |
| `create_network(name, labels)` / `create_network(spec)` | ✅ | ✅ | ✅ via Network (Networks.*) | ✅ via Network (WindowsNetworks.*) |
| `connect_network(net, id, aliases)` | ✅ | ✅ | ❌ [a] | ❌ [a] |
| `disconnect_network(net, id, force)` | ✅ | ✅ | ❌ [b] | ❌ |
| `remove_network(id)` | ✅ | ✅ | ✅ via Network (Networks.CreateAndRemove) | ✅ via Network (WindowsNetworks.CreateAndRemove) |
| `create_volume(spec)` | ✅ | ✅ | ✅ via Volume (Volumes.*) | ✅ via Volume (WindowsVolumes.*) |
| `inspect_volume(name)` | ✅ | ✅ | ✅ Volumes.CreateInspectRemove (direct + via handle) | ✅ WindowsVolumes.CreateInspectRemove |
| `remove_volume(name, force)` | ✅ | ✅ | ✅ via Volume (Volumes.*) | ✅ via Volume (WindowsVolumes.*) |
| `Response::header/ok` | ✅ | ✅ | ✅ ReaperTest, HostAccess (status_code) | ✅ WindowsEngine |

Notes:
- [a] `connect_network` is only reachable through `Network::connect`, which has
  no test (containers join at create time). The host-access sidecar joins a
  network internally but not through the public `connect_network`.
- [b] `disconnect_network` is exercised only internally by the host-access
  sidecar teardown, not through a public-API test.

---

## DockerHost / Timeouts / transport

| Function | Works on Linux | Works on Windows | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|---|---|
| `DockerHost::resolve()` | ✅ | ✅ | ✅ implicit (from_environment everywhere) | ✅ implicit |
| `DockerHost::parse(url)` | ✅ | ✅ | ✅ TlsTransport.* (no daemon needed) | ✅ (platform-agnostic; unit + TlsTransport) |
| `scheme()` / `path()` / `hostname()` / `port()` / `http_host()` | ✅ | ✅ | ✅ Exec.FeedsStdin (scheme routing) | ✅ WindowsExec.FeedsStdin (scheme routing) |
| TLS transport (`connect` https) | ✅ | ✅ | ✅ TlsTransport.HttpsSchemeIsWired, TlsTransport.RealHandshakeRoundTrip (in-process TLS server) | ✅ (same tests; daemon-independent) |
| `TransportTimeouts` | ✅ | ✅ | ❌ (unit-tested) | ❌ |

TLS is exercised against an in-process self-signed server, not a real remote
daemon (see feature-notes: end-to-end TLS against a real daemon is not
CI-verified). The npipe (Windows) and unix-socket (Linux) transports are
exercised by every daemon-touching test in their respective modes; TCP is the
default CI transport shape for the named-pipe/socket cases.

---

## Error types

| Type | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|
| `Error` (base) | ✅ (base of the others) | ✅ |
| `DockerError` | ✅ RedisMvp (post-teardown inspect throws), BuildImage.BuildFailureThrows, Copy.ReadFileRejectsDirectory, Exec.StdinThrowsOnNonHalfClosableTransport | ✅ WindowsBuildImage.BuildFailureThrows, WindowsCopy.ReadFileRejectsDirectory |
| `NotFoundError` | ❌ [a] | ❌ |
| `TransportTimeoutError` | ❌ (unit-tested) | ❌ |
| `StartupTimeoutError` | ✅ WaitStrategies.TimeoutThrowsStartupTimeoutError (asserts it is NOT a DockerError) | ❌ |

Notes:
- [a] 404s are asserted only as `DockerError` / `std::exception` (e.g.
  `inspect_volume` on a removed volume), never as the `NotFoundError` subtype
  specifically.

---

## ContainerRequest / run()

| Function | Works on Linux | Works on Windows | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|---|---|
| `ContainerRequest` struct | ✅ | ✅ | ❌ (unit-tested; built by `to_request()`) | ❌ |
| `run(request)` | ✅ | ✅ | ✅ indirect — `GenericImage::start()` is `run(to_request())` | ✅ indirect (WindowsContainer.*) |
| `run(client, request)` | ✅ | ✅ | ❌ (no test builds a request and calls `run` directly) | ❌ |

No integration test constructs a `ContainerRequest` by hand and calls `run()`
directly; the path is covered only transitively through `GenericImage::start()`.

---

## Gaps worth closing

Prioritized shortlist of the most valuable missing integration tests, per engine.

### Windows engine (biggest coverage deficit)

1. **A published/mapped port + a non-exit wait strategy.** The single largest
   hole: no Windows test publishes a port, calls `get_host_port` /
   `first_mapped_port`, or gates on `listening_port` / `http` / `log`. A tiny
   Windows server image (even a nanoserver HTTP listener) would unlock the entire
   port-getter + wait-strategy surface at once (TODO.md flags this).
2. **Lifecycle hooks and startup retry on Windows** (`with_*_hook`,
   `with_startup_attempts`) — orchestration logic that is engine-independent in
   principle but only ever run on Linux.
3. **A log wait on Windows** (`wait_for::log` / `stdout_message`) — currently
   only the exit wait is exercised in Windows mode.
4. **Container-level `with_tty` and `with_working_dir` on Windows** — only their
   exec-level equivalents are covered.

### Linux engine

1. **Bind mounts (`Mount::bind`).** No integration coverage in either engine;
   the most common real-world mount type is untested. (tmpfs and volume mounts
   are covered.)
2. **`Network::connect` / `create_container` auth-less paths** — attaching an
   already-running container via the public `connect()` (and by extension
   `connect_network` / `disconnect_network`) is never tested.
3. **Build context beyond an inline Dockerfile** — `with_dockerfile(path)`,
   `with_file`, `with_data`, `with_build_arg`, `with_target`, `with_no_cache`,
   `with_pull` on `GenericBuildableImage` have no integration coverage.
4. **`with_registry_auth` at the `GenericImage` level** and a real private-image
   pull — AuthTest only covers the public-image path and the credential-helper
   subprocess; the actual authenticated pull is not asserted end-to-end.
5. **`NotFoundError` as a distinct type** — 404s are only caught as `DockerError`.
6. **Value-type knobs**: `CopyToContainer::with_mode`, `ExecOptions.privileged`,
   `with_memory_limit` / `with_shm_size` / `with_cap_add` / `with_cap_drop`, and
   the Volume/Network `Builder` options (driver opts, labels, gateway, internal)
   — each created but never asserted against a running container.
