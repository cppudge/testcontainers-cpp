#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "testcontainers/Container.hpp"
#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/Lifecycle.hpp"
#include "testcontainers/RegistryAuth.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers {

/// When to pull the image before creating the container.
enum class ImagePullPolicy {
    Default, ///< pull only if missing locally (lazy, on create 404)
    Always,  ///< always pull before create (even if present locally)
};

/// The complete "what to run" for one container: the Docker create spec plus
/// every run-time input the start orchestration needs (readiness conditions,
/// files to copy in, lifecycle hooks, pull/reuse/retry policy).
///
/// A plain copyable value, normally assembled by `GenericImage::to_request()`
/// and consumed by `run()`. A caller with run-level needs (a custom
/// DockerClient, a spec tweak no builder exposes) can edit or build one
/// directly — the fields mirror the `GenericImage` builders one-to-one. If you
/// build one by hand rather than via `to_request()`, you own the consistency
/// of the port trio: `exposed_ports` (typed, declaration order),
/// `spec.exposed_ports` (the rendered "6379/tcp" strings), and
/// `spec.published_ports` (the explicit ephemeral bindings).
struct ContainerRequest {
    /// The Docker create body, fully translated: the image reference is already
    /// resolved/substituted, env entries are "KEY=VALUE", exposed ports are
    /// "6379/tcp" strings. Session/reuse labels are layered on by `run()` (they
    /// depend on the run, not the request).
    CreateContainerSpec spec;

    /// The declared exposed ports in declaration order. The handle remembers
    /// them so `Container::first_mapped_port()` resolves the FIRST declared
    /// port; the rendered strings for the create body live in
    /// `spec.exposed_ports`.
    std::vector<ContainerPort> exposed_ports;

    /// Files / bytes copied into the container between create and start.
    std::vector<CopyToContainer> copy_to_sources;

    /// Host (test-process) ports the container must be able to reach via
    /// `host.testcontainers.internal` (see `GenericImage::with_exposed_host_port`).
    /// When non-empty, `run()` starts the process-wide sshd sidecar, forwards
    /// these ports through it, and appends the alias's ExtraHosts entry to
    /// `spec` for this run.
    std::vector<std::uint16_t> host_access_ports;

    /// Readiness conditions, run in order under one shared `startup_timeout`.
    std::vector<WaitFor> waits;
    std::chrono::milliseconds startup_timeout{std::chrono::seconds(60)};

    /// Explicit registry credentials for pulls. When unset, credentials are
    /// auto-resolved from the Docker config for the image's registry.
    std::optional<RegistryAuth> registry_auth;

    ImagePullPolicy pull_policy = ImagePullPolicy::Default;

    /// Age budget for the LOCAL image under `ImagePullPolicy::Default`: when
    /// set, the run pulls first if the local copy's Created timestamp is
    /// older than this — or cannot be read. A missing image needs no pull
    /// here (create fetches it lazily), and `Always` makes this moot (it
    /// pulls regardless). Caveat: Created is the image's BUILD time, not the
    /// pull time, so an image built long ago re-pulls on every start even
    /// when the registry holds the same bytes.
    std::optional<std::chrono::seconds> pull_max_age;

    /// Opt-in container reuse; only effective when reuse is also enabled
    /// globally (see `GenericImage::with_reuse` for the full semantics).
    bool reuse = false;

    std::vector<LifecycleHook> created_hooks;  ///< after create, before copy/start
    std::vector<LifecycleHook> starting_hooks; ///< after copy-to, before start
    std::vector<LifecycleHook> started_hooks;  ///< after wait-until-ready
    std::vector<LifecycleHook> stopping_hooks; ///< fired by the Container at teardown

    int startup_attempts = 1; ///< create→start→wait attempts; values < 1 mean 1
};

/// Create, start, and wait for a container described by `request`, returning a
/// RAII handle that removes the container on destruction (unless the request
/// reused a persistent container). Boots the crash-safety reaper and connects
/// to the daemon from the environment (DOCKER_HOST etc.) — this is exactly what
/// `GenericImage::start()` runs. Throws on failure, best-effort removing a
/// container that was created but never became ready.
Container run(const ContainerRequest& request);

/// As above, but on a caller-supplied client — for a custom daemon endpoint or
/// tuned transport timeouts. The reaper is booted on `client`'s daemon (not the
/// environment one); reapers are per-daemon, keyed by endpoint URL, so a
/// second daemon used in the same process gets its own crash-safe reaper (two
/// URL spellings of one daemon count as two endpoints — harmless, two
/// reapers). The client is copied into the returned handle.
Container run(DockerClient client, const ContainerRequest& request);

} // namespace testcontainers
