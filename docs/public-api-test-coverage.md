# Public API integration-test coverage

Audit date: 2026-07-05. Updated the same day after the gap-closing test batch
(verified live: Linux engine and Windows engine CI runs green).

This file audits the integration-test coverage of every public interface under
`include/testcontainers/` against a real Docker daemon, in each of the two engine
modes. It is a companion to [feature-notes.md](feature-notes.md) (what
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
| `exists(name, tag)` (static) | ✅ | ✅ | ✅ BuildImage.ExistsReflectsLocalImages | ✅ WindowsBuildImage.ExistsAndBuildLogConsumer |
| `inspect(name, tag)` (static) | ✅ | ✅ | ✅ BuildImage.InspectReflectsImageConfig | ✅ WindowsBuildImage.ExistsAndBuildLogConsumer |
| `inspect()` | ✅ | ✅ | ✅ BuildImage.InspectReflectsImageConfig | ❌ (same code path as the static) |
| `start()` | ✅ | ✅ | ✅ RedisMvp.StartsConnectsAndAutoRemoves (+ most suites) | ✅ WindowsContainer.EchoExitsWithExpectedLogs |
| `to_request()` | ✅ | ✅ | ❌ (unit-tested; every `start()` uses it internally) | ❌ |
| `with_exposed_port` | ✅ | ✅ | ✅ RedisMvp, PortGetters.*, WaitStrategies.* | ✅ WindowsPortGetters.PublishedPortResolvesMappedPort |
| `with_env` | ✅ | ✅ | ❌ [a] | ❌ [a] |
| `with_cmd` | ✅ | ✅ | ✅ nearly every Linux suite | ✅ WindowsContainer.EchoExitsWithExpectedLogs |
| `with_entrypoint` | ✅ | ✅ | ✅ ContainerConfig.EntrypointOverride | ❌ |
| `with_working_dir` | ✅ | ✅ | ✅ ContainerConfig.WorkingDirAndUser | ✅ WindowsContainer.WorkingDirApplied |
| `with_user` | ✅ | ✅ | ✅ ContainerConfig.WorkingDirAndUser | ✅ WindowsVolumes.DataPersistsAcrossContainers |
| `with_privileged` | ✅ | ❌ (not supported) | ❌ | n/a |
| `with_isolation` | ❌ (daemon rejects non-default) | ✅ | n/a | ✅ implicit — `nanoserver()` sets `with_isolation("process")` for every Windows test |
| `with_tty` | ✅ | ✅ | ✅ Tty.LogsAreRawNotFramed, Tty.FollowLogsDeliversRaw | ✅ WindowsContainer.TtyLogsAreRaw |
| `with_mount` (bind) | ✅ | ✅ | ✅ ContainerConfig.BindMountReadOnly | ❌ |
| `with_mount` (volume) | ✅ | ✅ | ✅ Volumes.PopulateThenReadBack | ✅ WindowsVolumes.DataPersistsAcrossContainers |
| `with_mount` (tmpfs) | ✅ | ❌ (Linux-only) | ✅ ContainerConfig.TmpfsMount | n/a |
| `with_copy_to` | ✅ | ✅ | ✅ Copy.CopyAtStartData, Copy.CopyAtStartHostFile, Copy.CopyDirAtStart | ✅ WindowsCopy.CopyAtStartData, WindowsCopy.CopyAtStartHostFile, WindowsCopy.CopyDirAtStart |
| `with_label` | ✅ | ✅ | ❌ [b] | ❌ |
| `with_wait` | ✅ | ✅ | ✅ WaitStrategies.* (+ most suites) | ✅ WindowsWaitStrategies.* (+ WindowsContainer.EchoExitsWithExpectedLogs) |
| `with_startup_timeout` | ✅ | ✅ | ✅ WaitStrategies.TimeoutThrowsStartupTimeoutError, Lifecycle.StartupRetriesOnFailure | ✅ WindowsLifecycle.StartupRetriesOnFailure |
| `with_healthcheck` | ✅ | ✅ | ✅ WaitStrategies.HealthcheckWaitBecomesHealthy | ✅ WindowsWaitStrategies.HealthcheckWaitBecomesHealthy |
| `with_network` (name string or `Network` handle) | ✅ | ✅ | ✅ Networks.ResolvesPeerByContainerName (handle form; delegates to the string form) | ✅ WindowsNetworks.PeerNameRegisteredAndReachable |
| `with_network_alias` | ✅ | ✅ | ✅ Networks.AliasResolvesOnCustomNetwork | ✅ WindowsNetworks.AliasRegisteredOnCustomNetwork |
| `with_static_ipv4` | ✅ | ✅ | ✅ Networks.StaticIpv4Assigned | ❌ |
| `with_container_name` | ✅ | ✅ | ✅ Networks.ResolvesPeerByContainerName | ✅ WindowsNetworks.PeerNameRegisteredAndReachable |
| `with_platform` | ✅ | ✅ | ❌ | ❌ |
| `with_registry_auth` | ✅ | ✅ | ❌ [c] | ❌ |
| `with_memory_limit` | ✅ | ✅ | ✅ ContainerConfig.MemoryAndShmLimitsVisibleInside | ❌ |
| `with_shm_size` | ✅ | ❌ (Linux-only) | ✅ ContainerConfig.MemoryAndShmLimitsVisibleInside | n/a |
| `with_ulimit` | ✅ | ❌ (Linux-only) | ✅ ContainerConfig.UlimitApplied | n/a |
| `with_cap_add` | ✅ | ❌ (Linux-only) | ✅ ContainerConfig.CapAddDropReflectedInBounding | n/a |
| `with_cap_drop` | ✅ | ❌ (Linux-only) | ✅ ContainerConfig.CapAddDropReflectedInBounding | n/a |
| `with_extra_host` | ✅ | ✅ | ✅ ContainerConfig.ExtraHostApplied | ❌ |
| `with_cpu_limit` | ✅ | ✅ | ✅ ContainerConfig.CpuPidsCpusetLimitsVisibleInside | ❌ |
| `with_cpuset_cpus` | ✅ | ❌ (Linux-only) | ✅ ContainerConfig.CpuPidsCpusetLimitsVisibleInside | n/a |
| `with_pids_limit` | ✅ | ❌ (Linux-only) | ✅ ContainerConfig.CpuPidsCpusetLimitsVisibleInside | n/a |
| `with_restart_policy` | ✅ | ✅ | ✅ ContainerConfig.TypedHostConfigEchoedByInspect | ❌ |
| `with_dns_server` | ✅ | ✅ | ✅ ContainerConfig.DnsConfigWrittenToResolvConf | ❌ |
| `with_dns_search` | ✅ | ✅ | ✅ ContainerConfig.DnsConfigWrittenToResolvConf | ❌ |
| `with_dns_option` | ✅ | ✅ | ✅ ContainerConfig.DnsConfigWrittenToResolvConf | ❌ |
| `with_sysctl` | ✅ | ❌ (Linux-only) | ✅ ContainerConfig.SysctlAppliedInside | n/a |
| `with_device` | ✅ | ❌ (Linux path semantics; Windows uses class GUIDs) | ✅ ContainerConfig.DeviceMappedInside | n/a |
| `with_exposed_host_port` | ✅ | ❌ (throws; sshd sidecar is Linux) | ✅ HostAccess.* | n/a |
| `with_exposed_host_port` disabled build (`TC_HOST_PORT_FORWARDING=OFF`) | ✅ | ✅ (same refusal) | ✅ HostAccess.DisabledBuildThrowsClearError (CI: linux-minimal job) | n/a |
| `with_create_body_patch` | ✅ | ✅ | ❌ | ❌ |
| `with_image_pull_policy` | ✅ | ✅ | ✅ ContainerConfig.AlwaysPullPolicyStarts | ❌ |
| `with_image_pull_policy(max_age)` + `pull_max_age()` | ✅ | ✅ | ❌ (unit-tested: Runner.PullMaxAge* — stale pulls / fresh skips / unreadable pulls / missing stays lazy; GenericImage.PullPolicyOverloadsReplaceEachOther) | ❌ |
| `with_reuse` | ✅ | ✅ | ✅ Reuse.ReuseAdoptsRunningContainer, Reuse.ReuseDisabledCreatesFresh | ❌ |
| `with_image_name_substitutor` | ✅ | ✅ | ✅ ContainerConfig.CustomSubstitutorRewritesImage | ❌ |
| `with_created_hook` | ✅ | ✅ | ✅ Lifecycle.HooksFireInOrder | ✅ WindowsLifecycle.HooksFireInOrder |
| `with_starting_hook` | ✅ | ✅ | ✅ Lifecycle.HooksFireInOrder | ✅ WindowsLifecycle.HooksFireInOrder |
| `with_started_hook` | ✅ | ✅ | ✅ Lifecycle.HooksFireInOrder | ✅ WindowsLifecycle.HooksFireInOrder |
| `with_stopping_hook` | ✅ | ✅ | ✅ Lifecycle.StoppingHookFiresOnStop | ❌ |
| `with_startup_attempts` | ✅ | ✅ | ✅ Lifecycle.StartupRetriesOnFailure | ✅ WindowsLifecycle.StartupRetriesOnFailure |
| getters (`image()`, `env()`, …) | ✅ | ✅ | unit-tested | unit-tested |

Notes:
- [a] `with_env` is passed on every start and is used as a reuse-hash marker in
  the Reuse suite, but no test asserts the variable is visible *inside* the
  container. (Exec.PassesEnv covers `ExecOptions.env`, a different path.)
- [b] User labels are never asserted; ReaperTest asserts the *session* labels
  the runner injects, not `with_label` values.
- [c] `with_registry_auth` is untested at the `GenericImage` level. AuthTest
  exercises the credential path through `DockerClient::pull_image` instead.

---

## GenericBuildableImage (build from Dockerfile)

| Function | Works on Linux | Works on Windows | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|---|---|
| `GenericBuildableImage(name, tag)` | ✅ | ✅ | ✅ BuildImage.* | ✅ WindowsBuildImage.* |
| `with_dockerfile(path)` | ✅ | ✅ | ✅ BuildImage.DockerfilePathAndTargetStage | ❌ |
| `with_dockerfile_string` | ✅ | ✅ | ✅ BuildImage.BuildsAndRunsInlineDockerfile | ✅ WindowsBuildImage.BuildsAndRunsInlineDockerfile |
| `with_file(path, target)` | ✅ | ✅ | ✅ BuildImage.ContextFilesBuildArgsNoCache | ❌ |
| `with_data(bytes, target)` | ✅ | ✅ | ✅ BuildImage.ContextFilesBuildArgsNoCache | ❌ |
| `with_build_arg` | ✅ | ✅ | ✅ BuildImage.ContextFilesBuildArgsNoCache | ❌ |
| `with_target` | ✅ | ✅ | ✅ BuildImage.DockerfilePathAndTargetStage | ❌ |
| `with_no_cache` | ✅ | ✅ | ✅ BuildImage.ContextFilesBuildArgsNoCache | ❌ |
| `with_pull` | ✅ | ✅ | ✅ BuildImage.DockerfilePathAndTargetStage | ❌ |
| `with_build_log_consumer` | ✅ | ✅ | ✅ BuildImage.BuildLogConsumerStreamsSteps | ✅ WindowsBuildImage.ExistsAndBuildLogConsumer |
| `build()` | ✅ | ✅ | ✅ BuildImage.BuildsAndRunsInlineDockerfile, BuildImage.BuildFailureThrows, BuildImage.BuildFailureCarriesStepOutput | ✅ WindowsBuildImage.BuildsAndRunsInlineDockerfile, WindowsBuildImage.BuildFailureThrows |
| `build()` session labels + reaper boot | ✅ | ✅ (labels applied; no reaper on the Windows engine) | ✅ BuildImage.BuiltImageCarriesSessionLabels | ❌ (labels applied, unwatched) |
| `descriptor()`, getters | ✅ | ✅ | unit-tested | unit-tested |

The full builder surface (host-path Dockerfile, file/dir/in-memory context,
build args, target stage, no-cache, pull) is now covered on Linux
(BuildImage.ContextFilesBuildArgsNoCache, BuildImage.DockerfilePathAndTargetStage);
on Windows only the inline-Dockerfile + build-error round-trip is exercised.

---

## Container (RAII handle)

| Function | Works on Linux | Works on Windows | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|---|---|
| `adopt(client, id, ownership, tty)` | ✅ | ✅ | ❌ | ❌ |
| `id()` | ✅ | ✅ | ✅ RedisMvp, Reuse, Networks | ✅ WindowsNetworks.* (`srv.id()`) |
| `is_persistent()` | ✅ | ✅ | ✅ Reuse.ReuseAdoptsRunningContainer | ❌ |
| `has_tty()` | ✅ | ✅ | ❌ (unit path via logs) | ❌ |
| `host()` | ✅ | ✅ | ✅ RedisMvp.StartsConnectsAndAutoRemoves | ❌ |
| `get_host_port` | ✅ | ✅ | ✅ RedisMvp, WaitStrategies.*, PortGetters.* | ✅ WindowsPortGetters.PublishedPortResolvesMappedPort |
| `get_host_port_ipv4` | ✅ | ✅ | ✅ PortGetters.Ipv4AndDefaultAgree | ✅ WindowsPortGetters.PublishedPortResolvesMappedPort |
| `get_host_port_ipv6` | ? (daemon-dependent) | ? | ✅ PortGetters.Ipv4AndDefaultAgree (tolerant: resolves or throws) | ❌ |
| `first_mapped_port` | ✅ | ✅ | ✅ PortGetters.FirstMappedPicksExposedOrder | ✅ WindowsPortGetters.PublishedPortResolvesMappedPort |
| `inspect()` | ✅ | ✅ | ✅ PortGetters.InspectAndRaw, ContainerConfig.TypedHostConfigEchoedByInspect (`host_config` echo) | ✅ WindowsPortGetters.PublishedPortResolvesMappedPort (`inspect().ports`) |
| `inspect_raw()` | ✅ | ✅ | ✅ PortGetters.InspectAndRaw | ❌ [a] |
| `logs()` | ✅ | ✅ | ✅ ContainerConfig.*, Tty.LogsAreRawNotFramed | ✅ WindowsContainer.EchoExitsWithExpectedLogs, WindowsBuildImage.* |
| `follow_logs()` | ✅ | ✅ | ✅ Tty.FollowLogsDeliversRaw | ❌ |
| `exec(cmd)` | ✅ | ✅ | ✅ Exec.CapturesStdoutAndZeroExit | ✅ WindowsContainer.ExecRunsInRunningContainer |
| `exec(cmd, opts)` | ✅ | ✅ | ✅ Exec.PassesEnv/UsesWorkingDir/RunsAsUser/TtyCapturesRawStdout/FeedsStdin/LargeStdinEchoRoundTrip/ConsoleSizeAppliesToTtyExec/ResizeExecAppliesMidRun | ✅ WindowsExec.* |
| `exec(cmd, opts, consumer)` | ✅ | ✅ | ✅ Exec.StreamsOutputIncrementally, Exec.StreamingStopsWhenConsumerReturnsFalse | ✅ WindowsExec.StreamsOutputIncrementally, WindowsExec.StreamingStopsWhenConsumerReturnsFalse |
| `exec(cmd, opts, consumer, deadline)` | ✅ | ✅ | ✅ Exec.DeadlineBoundedStreamingReportsExpiry, Exec.DeadlineBoundedStreamingCompletesInTime | ✅ WindowsExec.DeadlineBoundedStreamingReportsExpiry |
| `copy_to(source)` | ✅ | ✅ [b] | ✅ Copy.CopyIntoRunningContainer | ✅ WindowsCopy.CopyIntoRunningContainer |
| `read_file(path)` | ✅ | ✅ [b] | ✅ Copy.ReadFileRoundTrip, Copy.LargeFileRoundTrip, Copy.ReadFileRejectsDirectory | ✅ WindowsCopy.ReadFileRoundTrip, WindowsCopy.LargeFileRoundTrip, WindowsCopy.ReadFileRejectsDirectory |
| `copy_file_from(path, host)` | ✅ | ✅ [b] | ✅ Copy.CopyFileFromWritesHost | ✅ WindowsCopy.CopyFileFromWritesHost |
| `resize_tty(size)` | ✅ | ✅ | ✅ Tty.ResizeTtyChangesWindowSize | ❌ (ConPTY resize untested; Linux path covers the client-side wire) |
| `stop()` | ✅ | ✅ | ✅ Lifecycle.StoppingHookFiresOnStop | ❌ |
| `is_running()` | ✅ | ✅ | ✅ RedisMvp, WaitStrategies.* | ✅ WindowsContainer.ExecRunsInRunningContainer |
| `keep(bool)` | ✅ | ✅ | ✅ Lifecycle.KeepLeavesContainerRunning (+ unit Runner.KeepSkipsRemovalOnDrop, Runner.KeepFalseRearmsRemovalOnDrop) | ❌ (client-side flag; engine-independent) |
| `inspect(id)` (static) | ✅ | ✅ | ✅ Lifecycle.StaticInspectById (found + NotFoundError) | ❌ (same code path as `inspect_container`) |
| `remove()` | ✅ | ✅ | ✅ implicit via RAII drop everywhere | ✅ implicit via RAII drop |

Notes:
- [a] `inspect()` is now covered on Windows (WindowsPortGetters), but
  `inspect_raw()` is still only called via raw `request()` in the Windows
  network tests, not through the Container handle.
- [b] Windows filesystem ops require **process** isolation; Docker Desktop's
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
| `keep(bool)` / `is_persistent()` | ✅ | ✅ | ✅ Networks.KeepReleasesRemovalOwnership | ❌ (client-side flag; engine-independent) |
| `inspect()` | ✅ | ✅ | ✅ Networks.InspectReportsConfigAndContainers, Networks.BuilderInternalGatewayAndLabels | ✅ WindowsNetworks.InspectReportsDriverAndContainers |
| `inspect_raw()` | ✅ | ✅ | ✅ Networks.InspectReportsConfigAndContainers | ❌ (same code path) |
| `inspect(name_or_id)` (static) | ✅ | ✅ | ✅ Networks.InspectReportsConfigAndContainers (by name) | ✅ WindowsNetworks.InspectReportsDriverAndContainers (by name) |
| `connect(id, aliases)` | ✅ | ✅ | ✅ Networks.ConnectAttachesRunningContainerWithAlias | ❌ [a] |
| `builder()` + `create()` | ✅ | ✅ | ✅ Networks.BuilderCreatesNetwork | ✅ WindowsNetworks.BuilderCreatesNetwork |
| `Builder::with_driver` | ✅ | ✅ | ✅ Networks.BuilderCreatesNetwork ("bridge") | ✅ WindowsNetworks.BuilderCreatesNetwork ("nat") |
| `Builder::with_attachable` | ✅ | ❌ (HNS rejects) | ✅ Networks.BuilderCreatesNetwork | n/a |
| `Builder::with_subnet` | ✅ | ✅ | ✅ Networks.BuilderCreatesNetwork | ✅ WindowsNetworks.BuilderCreatesNetwork |
| `Builder::with_name` | ✅ | ✅ | ❌ | ❌ |
| `Builder::with_internal` | ✅ | ✅ | ✅ Networks.BuilderInternalGatewayAndLabels | ❌ |
| `Builder::with_enable_ipv6` | ✅ | ? | ❌ | ❌ |
| `Builder::with_gateway` | ✅ | ✅ | ✅ Networks.BuilderInternalGatewayAndLabels | ❌ |
| `Builder::with_option` | ✅ | ✅ | ❌ | ❌ |
| `Builder::with_label` | ✅ | ✅ | ✅ Networks.BuilderInternalGatewayAndLabels | ❌ |

Notes:
- [a] `Network::connect` (attach an *already-running* container) is now covered
  on Linux (Networks.ConnectAttachesRunningContainerWithAlias) but not on
  Windows — the Windows network tests still attach only at create time via
  `GenericImage::with_network`.

---

## Volume (RAII handle + Builder)

| Function | Works on Linux | Works on Windows | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|---|---|
| `Volume::create(name)` | ✅ | ✅ | ✅ Networks/Volumes via name path | ❌ (only `create()` used) |
| `Volume::create()` | ✅ | ✅ | ✅ Volumes.CreateInspectRemove, Volumes.RaiiRemovesOnDrop, Volumes.PopulateThenReadBack | ✅ WindowsVolumes.CreateInspectRemove, WindowsVolumes.DataPersistsAcrossContainers |
| `name()` | ✅ | ✅ | ✅ Volumes.CreateInspectRemove | ✅ WindowsVolumes.CreateInspectRemove |
| `remove()` | ✅ | ✅ | ✅ Volumes.CreateInspectRemove, Volumes.RaiiRemovesOnDrop | ✅ WindowsVolumes.CreateInspectRemove |
| `inspect()` | ✅ | ✅ | ✅ Volumes.CreateInspectRemove | ✅ WindowsVolumes.CreateInspectRemove |
| `populate(sources, …)` | ✅ | ✅ (staged: layer copy + in-container xcopy, 2026-07-11) | ✅ Volumes.PopulateThenReadBack, Volumes.PopulateDirSource | ✅ WindowsVolumes.PopulateSeedsVolume [a] |
| `builder()` + `create()` | ✅ | ✅ | ✅ Volumes.BuilderSetsNameAndLabels | ❌ |
| `Builder::with_name` / `with_label` | ✅ | ✅ | ✅ Volumes.BuilderSetsNameAndLabels | ❌ |
| `Builder::with_driver` / `with_driver_opt` | ✅ | ✅ | ❌ | ❌ |

Notes:
- [a] WindowsVolumes.DataPersistsAcrossContainers pins the underlying mechanism
  (a write from inside a mounted container goes through the junction and
  persists) that populate()'s staged Windows path builds on.

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
| `with_exposed_service` (+ per-instance overload) | ✅ | n/a | ✅ Compose.* (per-instance: ScaleRunsTwoInstances) | n/a |
| `with_profile` | ✅ | n/a | ✅ Compose.ProfileGatesService (gated service down without / up with; profile-aware teardown) | n/a |
| `with_scale` | ✅ | n/a | ✅ Compose.ScaleRunsTwoInstances (two live instances, distinct ports, PING both) | n/a |
| `with_ambassador` | ✅ | n/a | ✅ Compose.AmbassadorReachesUnpublishedPort (PONG through the relay; service publishes nothing; network removed cleanly) | n/a |
| `with_ambassador_image` | ✅ | n/a | ❌ (default alpine/socat:1.8.0.3 used) | n/a |
| `with_project_name` | ✅ | n/a | ❌ | n/a |
| `with_compose_image` | ✅ | n/a | ❌ (default docker:26.1-cli used) | n/a |
| `with_env` / `with_env_vars` | ✅ | n/a | ❌ | n/a |
| `with_build` | ✅ | n/a | ❌ | n/a |
| `with_pull` | ✅ | n/a | ❌ | n/a |
| `with_wait` | ✅ | n/a | ❌ (default on; not toggled) | n/a |
| `with_wait_timeout` | ✅ | n/a | ❌ | n/a |
| `with_remove_volumes` / `with_remove_images` | ✅ | n/a | ❌ | n/a |
| `start()` | ✅ | n/a | ✅ Compose.* (+ restart: RestartKeepsProjectAlive) | n/a |
| `start()` Ryuk project filter | ✅ | n/a | ✅ Compose.ProjectFilterRegisteredWithReaper (registered once, ACKed by the real Ryuk) | n/a |
| `stop()` | ✅ | n/a | ✅ Compose.* (+ label sweep assertion) | n/a |
| `get_service_host` | ✅ | n/a | ✅ Compose.* | n/a |
| `get_service_port` (+ per-instance overload) | ✅ | n/a | ✅ Compose.* (per-instance: ScaleRunsTwoInstances) | n/a |
| `get_service_container_id` (+ per-instance overload) | ✅ | n/a | ✅ Compose.RestartKeepsProjectAlive, Compose.ScaleRunsTwoInstances | n/a |
| `service_instances` | ✅ | n/a | ✅ Compose.ScaleRunsTwoInstances | n/a |
| `get_service_logs` (+ per-instance overload) | ✅ | n/a | ✅ Compose.ServiceLogsDeliverRedisStartup (both forms) | n/a |
| `follow_service_logs` (blocking + deadline, per-instance forms) | ✅ | n/a | ✅ Compose.ServiceLogsDeliverRedisStartup (deadline form; blocking form delegates to the same impl) | n/a |
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
strategy. Windows coverage now spans exit, log, healthcheck, listening-port
(the port test runs a PowerShell `TcpListener` in a servercore container), and
the command wait.

| Strategy | Works on Linux | Works on Windows | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|---|---|
| `wait_for::None` (no wait) | ✅ | ✅ | ✅ implicit (Exec/Copy/Network containers start with no wait) | ✅ implicit (WindowsExec keep-alive containers) |
| `stdout_message` / `stderr_message` / `log` | ✅ | ✅ | ✅ RedisMvp (stdout), Tty.LogWaitWorksOnTtyContainer, WaitStrategies.TimeoutThrows (log) | ✅ WindowsWaitStrategies.StdoutMessageWait (stdout_message) |
| `seconds` / `millis` (Duration) | ✅ | ✅ | ❌ | ❌ |
| `exit` / `exit_code` | ✅ | ✅ | ✅ WaitStrategies.ExitCodeWaitSucceeds, BuildImage.* | ✅ WindowsWaitStrategies.ExitCodeWaitSucceeds, WindowsContainer.EchoExitsWithExpectedLogs, WindowsBuildImage.* |
| `healthy` (Healthcheck) | ✅ | ✅ | ✅ WaitStrategies.HealthcheckWaitBecomesHealthy | ✅ WindowsWaitStrategies.HealthcheckWaitBecomesHealthy |
| `http` | ✅ | ✅ [a] | ✅ WaitStrategies.HttpWaitReachesNginx | ❌ [a] |
| `listening_port` (Port) | ✅ | ✅ | ✅ WaitStrategies.PortWaitReachesRedis | ✅ WindowsWaitStrategies.ListeningPortWaitOnServercore |
| `successful_command` / `successful_shell_command` (Command) | ✅ | ✅ | ✅ WaitStrategies.CommandWaitReachesRedis / .CommandWaitRetriesUntilFlagAppears / .CommandWaitTimeoutCarriesExitCode (retry/error paths also wire-pinned: CommandWait.* units) | ✅ WindowsWaitStrategies.CommandWaitRetriesUntilFlagAppears |

Notes:
- [a] The HTTP strategy works against a Windows daemon in principle
  (listening-port is now confirmed live), but no Windows test image serves
  HTTP yet — it needs a real HTTP server image, not just a TcpListener.

---

## Healthcheck / Mount / CopyToContainer / ExecOptions / ExecResult (value types)

These are copyable value types; their behavior is verified through the modules
that consume them (rows above). Summary of where each is exercised:

| Type | Integration-tested (Linux) | Integration-tested (Windows) |
|---|---|---|
| `Healthcheck` (cmd_shell, interval/retries/start_period) | ✅ WaitStrategies.HealthcheckWaitBecomesHealthy | ✅ WindowsWaitStrategies.HealthcheckWaitBecomesHealthy |
| `Healthcheck::cmd` / `::none` | ❌ (unit-tested) | ❌ |
| `Mount::bind` (+ `with_read_only`) | ✅ ContainerConfig.BindMountReadOnly | ❌ |
| `Mount::volume` | ✅ Volumes.PopulateThenReadBack | ✅ WindowsVolumes.DataPersistsAcrossContainers |
| `Mount::tmpfs` (+ size/mode) | ✅ ContainerConfig.TmpfsMount | n/a (Linux-only) |
| `CopyToContainer::content` | ✅ Copy.CopyAtStartData | ✅ WindowsCopy.CopyAtStartData |
| `CopyToContainer::host_file` | ✅ Copy.CopyAtStartHostFile | ✅ WindowsCopy.CopyAtStartHostFile |
| `CopyToContainer::host_dir` | ✅ Copy.CopyDirAtStart, Copy.CopyDirIntoRunningContainer | ✅ WindowsCopy.CopyDirAtStart |
| `CopyToContainer::with_mode` | ✅ Copy.ModeAppliedToCopiedFile | ❌ |
| `ExecOptions` (env/working_dir/user/tty/stdin_data) | ✅ Exec.* | ✅ WindowsExec.* |
| `ExecOptions.privileged` | ✅ Exec.PrivilegedExecExpandsCapabilities | ❌ |
| `ExecOptions.detach` | ✅ Exec.DetachedRunsInBackground, Exec.DetachedDoesNotWaitForCompletion (+ unit ExecWire.Detach*) | ✅ WindowsExec.DetachedRunsInBackground, WindowsExec.DetachedDoesNotWaitForCompletion |
| `ExecResult` (stdout/stderr/exit_code) | ✅ Exec.* | ✅ WindowsExec.* |

Bind mounts, copy modes, and privileged exec are now Linux-covered; none of the
three has a Windows-mode test (file modes and privileged exec are Unix concepts
— on Windows both are effectively n/a, bind mounts are not).

---

## Lifecycle hooks

Covered under GenericImage above: on Linux (Lifecycle.HooksFireInOrder,
Lifecycle.StoppingHookFiresOnStop, Lifecycle.StartupRetriesOnFailure) and now
on Windows (WindowsLifecycle.HooksFireInOrder,
WindowsLifecycle.StartupRetriesOnFailure). The `LifecycleHook` typedef itself
has no separate surface. The stopping hook remains Linux-only-tested.

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
| `set/pull_retry` | ✅ | ✅ | ❌ (unit-tested: PullRetryTest) | ❌ |
| `set/max_response_body` | ✅ | ✅ | ❌ (unit-tested: BodyLimitTest) | ❌ |
| `request(method, target, …)` | ✅ | ✅ | ✅ ReaperTest, NetworkTest (raw inspect) | ✅ WindowsEngine `/version`, WindowsNetworks raw inspect |
| `ping()` | ✅ | ✅ | ✅ EngineGuard (every suite's SetUp) | ✅ EngineGuard |
| `server_os()` | ✅ | ✅ | ✅ WindowsEngine tag resolution | ✅ WindowsEngine (implicitly, via is_windows_engine) |
| `is_windows_engine()` | ✅ | ✅ | ✅ EngineGuard | ✅ EngineGuard |
| `pull_image(image, auth?)` | ✅ | ✅ | ✅ DockerLifecycle, AuthTest, DockerLogs | ❌ (pull happens via create in Windows tests) |
| `image_exists(reference)` | ✅ | ✅ | ✅ via GenericImage::exists (BuildImage.ExistsReflectsLocalImages) | ✅ via GenericImage::exists (WindowsBuildImage.ExistsAndBuildLogConsumer) |
| `inspect_image(reference)` | ✅ | ✅ | ✅ via GenericImage::inspect (BuildImage.InspectReflectsImageConfig) | ✅ via GenericImage::inspect (WindowsBuildImage.ExistsAndBuildLogConsumer) |
| `inspect_image_raw(reference)` | ✅ | ✅ | ✅ BuildImage.InspectReflectsImageConfig (direct; also under every `inspect_image`) | ✅ implicit — every `inspect_image` goes through it |
| `build_image(tar, opts[, consumer])` | ✅ | ✅ | ✅ via GenericBuildableImage (BuildImage.*) | ✅ via GenericBuildableImage (WindowsBuildImage.*) |
| `build_image(producer, opts[, consumer])` | ✅ | ✅ | ✅ every GenericBuildableImage build streams through it (also wire-tested: BuildWireTest) | ✅ same path (WindowsBuildImage.*) |
| `BuildOptions::labels` (`?labels=`) | ✅ | ✅ | ✅ BuildImage.BuiltImageCarriesSessionLabels (unit: ApiMapping.BuildQueryLabels) | ❌ |
| `create_container(spec, auth?)` | ✅ | ✅ | ✅ DockerLifecycle.*, ReaperTest, DockerLogs | ❌ (Windows tests go through `start()`) |
| `start_container(id)` | ✅ | ✅ | ✅ DockerLifecycle.CreateStartInspectRemove | ❌ |
| `inspect_container(id)` | ✅ | ✅ | ✅ DockerLifecycle.*, Compose.RestartKeepsProjectAlive | ❌ (indirect via is_running) |
| `inspect_container_raw(id)` | ✅ | ✅ | ✅ via Container.inspect_raw (PortGetters) | ❌ |
| `list_containers(filters, all)` | ✅ | ✅ | ✅ Compose.* (project-label sweep) | ❌ |
| `stop_container(id, timeout?)` | ✅ | ✅ | ✅ DockerLifecycle.CreateStartInspectRemove | ❌ |
| `remove_container(id, force, vols)` | ✅ | ✅ | ✅ RemoveGuard, Reuse cleanup | ❌ (indirect via RAII) |
| `logs(id, opts)` | ✅ | ✅ | ✅ DockerLogs.FetchesStdoutAndStderr | ❌ (Container.logs used instead) |
| `follow_logs(id, opts, consumer)` | ✅ | ✅ | ✅ DockerLogs.FollowStreamsUntilExit, DockerLogs.FollowStopsEarly… | ❌ |
| `exec(id, cmd[, opts[, consumer[, deadline]]])` | ✅ | ✅ | ✅ via Container (Exec.*) | ✅ via Container (WindowsExec.*) |
| `resize_exec(exec_id, size)` | ✅ | ✅ | ✅ Exec.ResizeExecAppliesMidRun (+ unit ExecWire.ResizeExecPostsDimensions) | ❌ (same wire path; ConPTY resize untested) |
| `resize_container_tty(id, size)` | ✅ | ✅ | ✅ via Container (Tty.ResizeTtyChangesWindowSize) + unit ExecWire.ResizeContainerPostsDimensions | ❌ |
| `copy_to_container(id, source)` | ✅ | ✅ | ✅ via Container.copy_to (Copy.*) | ✅ via Container.copy_to (WindowsCopy.*) |
| `copy_to_container(id, sources)` (batched) | ✅ | ✅ | ✅ Copy.BatchedCopyLandsAllSources (also the runner's copy-at-start path) | ✅ via with_copy_to at start (WindowsCopy.CopyAtStart*) |
| `copy_from_container(id, path)` | ✅ | ✅ | ✅ via Container.read_file (Copy.*) | ✅ via Container.read_file (WindowsCopy.*) |
| `copy_from_container(id, path, sink)` | ✅ | ✅ | ❌ (wire-tested: CopyWireTest.SinkStreamsArchiveDownload) | ❌ |
| `copy_from_container_to(id, path, dest)` | ✅ | ✅ | ✅ Copy.CopyFromToDirectoryRoundTrip | ❌ |
| `container_path_stat(id, path)` | ✅ | ✅ | ✅ Copy.ContainerPathStat | ❌ |
| `create_network(name, labels)` / `create_network(spec)` | ✅ | ✅ | ✅ via Network (Networks.*) | ✅ via Network (WindowsNetworks.*) |
| `list_networks(filters)` | ✅ | ✅ | ✅ Networks.ListNetworksFindsByLabel (+ the reuse find-before-create) | ❌ (Linux-only, as with `list_containers`) |
| `connect_network(net, id, aliases)` | ✅ | ✅ | ✅ via Network.connect (Networks.ConnectAttachesRunningContainerWithAlias) | ❌ [a] |
| `disconnect_network(net, id, force)` | ✅ | ✅ | ❌ [b] | ❌ |
| `remove_network(id)` | ✅ | ✅ | ✅ via Network (Networks.CreateAndRemove) | ✅ via Network (WindowsNetworks.CreateAndRemove) |
| `inspect_network(id)` | ✅ | ✅ | ✅ via Network.inspect (Networks.InspectReportsConfigAndContainers) | ✅ via Network.inspect (WindowsNetworks.InspectReportsDriverAndContainers) |
| `inspect_network_raw(id)` | ✅ | ✅ | ✅ via Network.inspect_raw (Networks.InspectReportsConfigAndContainers) | ❌ (same code path) |
| `create_volume(spec)` | ✅ | ✅ | ✅ via Volume (Volumes.*) | ✅ via Volume (WindowsVolumes.*) |
| `inspect_volume(name)` | ✅ | ✅ | ✅ Volumes.CreateInspectRemove (direct + via handle) | ✅ WindowsVolumes.CreateInspectRemove |
| `remove_volume(name, force)` | ✅ | ✅ | ✅ via Volume (Volumes.*) | ✅ via Volume (WindowsVolumes.*) |
| `list_volumes(filters)` | ✅ | ✅ | ✅ Volumes.ListVolumesFindsByLabel | ❌ (Linux-only, as with `list_containers`) |
| `prune_volumes(filters)` | ✅ | ✅ | ✅ Volumes.PruneRemovesUnusedByLabel | ❌ (same wire path) |
| `Response::header/ok` | ✅ | ✅ | ✅ ReaperTest, HostAccess (status_code) | ✅ WindowsEngine |

Notes:
- [a] `connect_network` is covered on Linux via `Network::connect`
  (Networks.ConnectAttachesRunningContainerWithAlias); no Windows test attaches
  a running container to a network.
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
| TLS disabled build (`TC_TLS=OFF`) | ✅ | ✅ | ✅ TlsTransport.DisabledBuildThrowsClearError + unit TransportTimeout.TlsDisabledConnectThrowsNamedError (CI: linux-minimal job) | ✅ (same tests; daemon-independent) |
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
| `NotFoundError` | ✅ Volumes.CreateInspectRemove, Volumes.RaiiRemovesOnDrop (typed 404 asserted) | ✅ WindowsVolumes.CreateInspectRemove |
| `TransportTimeoutError` | ❌ (unit-tested) | ❌ |
| `StartupTimeoutError` | ✅ WaitStrategies.TimeoutThrowsStartupTimeoutError (asserts it is NOT a DockerError) | ❌ |

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

Prioritized shortlist of the most valuable missing integration tests, per
engine, after the 2026-07-05 gap-closing batch (published ports, lifecycle
hooks, log/health/port waits on Windows; bind mounts, HostConfig knobs, network
connect, build-context variants, typed 404s on Linux are all now covered).

### Windows engine

1. **`wait_for::http` on Windows** — the listening-port wait is covered via a
   servercore PowerShell `TcpListener`, but the HTTP probe needs a real HTTP
   server image (e.g. an IIS/servercore-based one) to gate on a 200.
2. **Bind mounts on Windows containers** (`Mount::bind` of a host directory at
   `C:\...`) — untested; interacts with the copy-to path normalization caveat
   (feature-notes.md, TODO.md).
3. **`with_stopping_hook` / `Container::stop()` on Windows** — the teardown-hook
   path is only ever exercised on Linux.
4. **Windows-mode reuse, pull policy, substitutor, `with_extra_host`,
   `with_entrypoint`** — engine-independent orchestration that still runs only
   on the Linux job; cheap mirrors once a suite exists to hold them.

### Linux engine

1. **`with_registry_auth` end-to-end private pull** — AuthTest covers the
   public-image and credential-helper paths only; a real authenticated pull
   needs a private registry (and push support) in CI.
2. **Container-level `with_label`** (both engines) — user labels are set but
   never read back from inspect; same for asserting `with_env` inside the
   container (only `ExecOptions.env` is asserted).
3. **Compose configuration setters** — `with_env(_vars)`, `with_build`,
   `with_pull`, `with_project_name`, `with_compose_image`, `with_wait_timeout`,
   and the teardown flags are never toggled in an integration run; Compose
   itself remains Linux-only.
4. **`run(client, request)` / hand-built `ContainerRequest` / `Container::adopt`**
   — the public escape hatches are covered only transitively through `start()`.
5. **Remaining builder options** — Network `Builder::with_name` /
   `with_enable_ipv6` / `with_option`; Volume `Builder::with_driver` /
   `with_driver_opt`; `with_platform`; `with_create_body_patch`.
6. **`TransportTimeoutError` against a real wedged endpoint** and end-to-end TLS
   to a real remote daemon (feature-notes.md records both as not CI-verified).
