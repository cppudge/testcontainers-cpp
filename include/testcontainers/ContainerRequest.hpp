#pragma once

#include <chrono>
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
/// `spec.publish_all_ports`.
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

    /// Readiness conditions, run in order under one shared `startup_timeout`.
    std::vector<WaitFor> waits;
    std::chrono::milliseconds startup_timeout{std::chrono::seconds(60)};

    /// Explicit registry credentials for pulls. When unset, credentials are
    /// auto-resolved from the Docker config for the image's registry.
    std::optional<RegistryAuth> registry_auth;

    ImagePullPolicy pull_policy = ImagePullPolicy::Default;

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
/// environment one); note it is process-global and binds to the FIRST daemon it
/// starts against — later runs against a different daemon in the same process
/// get labels but no crash-safe reaping. The client is copied into the
/// returned handle.
Container run(DockerClient client, const ContainerRequest& request);

} // namespace testcontainers
