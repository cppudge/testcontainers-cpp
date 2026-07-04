#include "Runner.hpp"

#include <algorithm>
#include <string>

#include "HostPortForwarding.hpp"
#include "Reaper.hpp"
#include "Reuse.hpp"
#include "WaitStrategies.hpp"
#include "docker/ApiMapping.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers {

namespace detail {

Container Runner::run(DockerClient& client, const ContainerRequest& request) {
    // Local copy of the create spec: the session/reuse labels layered on below
    // depend on this run, not on the request.
    CreateContainerSpec spec = request.spec;

    // ImagePullPolicy::Always pulls before create even when the image is present
    // locally; Default relies on create's lazy pull-on-404 path.
    if (request.pull_policy == ImagePullPolicy::Always) {
        client.pull_image(spec.image, request.registry_auth);
    }

    // Host-port exposure: make sure the sshd sidecar + SSH forwards are up and
    // point host.testcontainers.internal at the sidecar (an ExtraHosts entry
    // appended to this run's spec). Must happen before the reuse hash below —
    // reachability is part of the container's effective config. The sidecar's
    // own start() recurses through here with no host_access_ports, so this
    // cannot loop.
    if (!request.host_access_ports.empty()) {
        detail::HostPortForwarder::instance().wire(client, spec, request.host_access_ports);
    }

    // The shared create→copy→start→wait tail, returning a handle. `remove_on_drop`
    // is false for reusable containers (they must persist across runs). A throwing
    // created/starting/started hook is treated like any other failure here: it is
    // inside the try so the partial container is best-effort removed before the
    // exception propagates (no leak at any of the three points).
    const auto create_start_wait = [&](const CreateContainerSpec& s,
                                       bool remove_on_drop) -> Container {
        const std::string id = client.create_container(s, request.registry_auth);

        // Everything from here until the container is constructed must, on any
        // throw, best-effort remove the partially-created container so it never
        // leaks — including a throwing created/starting/started hook.
        try {
            // created hooks: container exists (id assigned), before copy-to/start.
            for (const LifecycleHook& hook : request.created_hooks) {
                if (hook) {
                    hook(client, id);
                }
            }

            // Copy files/data in after create, before start (the create→copy→start
            // order).
            for (const CopyToContainer& source : request.copy_to_sources) {
                client.copy_to_container(id, source);
            }

            // starting hooks: after copy-to, immediately before start.
            for (const LifecycleHook& hook : request.starting_hooks) {
                if (hook) {
                    hook(client, id);
                }
            }

            client.start_container(id);

            detail::wait_until_ready(client, id, request.waits, request.startup_timeout,
                                     spec.tty);

            // started hooks: after wait-until-ready, before constructing the handle.
            for (const LifecycleHook& hook : request.started_hooks) {
                if (hook) {
                    hook(client, id);
                }
            }
        } catch (...) {
            // A container that was created but failed to come up (copy, start,
            // readiness, or a throwing created/starting/started hook) must not leak.
            try {
                client.remove_container(id, /*force*/ true, /*remove_volumes*/ true);
            } catch (...) {
                ;
            }
            throw;
        }

        // The handle gets its own client copy so the caller's `client` stays
        // usable across startup-attempt retries (DockerClient is a stateless host
        // config — opening a fresh connection per call).
        return Container(client, id, remove_on_drop, spec.tty, request.stopping_hooks,
                         request.exposed_ports);
    };

    // Run an attempt-producing factory up to startup_attempts times: on a thrown
    // failure, if attempts remain, swallow and retry (each retry builds a brand-new
    // container via the factory); after the final attempt, rethrow the last error.
    const auto with_retry = [&](const auto& attempt) -> Container {
        const int attempts = std::max(1, request.startup_attempts);
        for (int i = 0;; ++i) {
            try {
                return attempt();
            } catch (...) {
                if (i + 1 >= attempts) {
                    throw; // final attempt failed: propagate the last error
                }
                // attempts remain: create_start_wait already removed the partial
                // container before throwing, so just retry with a fresh one.
            }
        }
    };

    // Reuse is a safety-gated opt-in: it only activates when enabled globally
    // (~/.testcontainers.properties or TESTCONTAINERS_REUSE_ENABLE); otherwise
    // request.reuse degrades to a normal (reaped, auto-removed) container.
    const bool use_reuse = request.reuse && detail::reuse_enabled();
    if (use_reuse) {
        // Canonical config for the hash: the create body WITHOUT any reuse/session
        // labels, plus the copy-to descriptors (so copied content participates).
        std::string canonical = docker::build_create_body(spec).dump();
        for (const CopyToContainer& s : request.copy_to_sources) {
            canonical +=
                "\n" + s.target() + "\n" + (s.is_file() ? s.host_path().string() : s.bytes());
        }
        const std::string hash = detail::reuse_hash(canonical);

        // Reuse containers are NOT session-reaped (they must survive); tag
        // managed-by + the reuse hash only (no session-id label, or Ryuk would
        // reap them).
        spec.labels.emplace_back("org.testcontainers.managed-by", "testcontainers");
        spec.labels.emplace_back(detail::reuse_hash_label(), hash);

        // Look for a running container already matching this hash.
        const auto matches = client.list_containers(
            {{"label", std::string(detail::reuse_hash_label()) + "=" + hash}}, /*all*/ false);
        for (const ContainerSummary& m : matches) {
            if (m.state == "running") {
                // Adopt it: wait for readiness, return a NON-removing handle. This
                // path is NOT retried — it adopts an already-running match rather
                // than creating anything.
                detail::wait_until_ready(client, m.id, request.waits, request.startup_timeout,
                                         spec.tty);
                return Container(client, m.id, /*remove_on_drop*/ false, spec.tty,
                                 request.stopping_hooks, request.exposed_ports);
            }
        }
        // No match: create a NEW reuse container (persistent, not reaped). This
        // creates, so it IS subject to startup-attempt retry.
        return with_retry([&] { return create_start_wait(spec, /*remove_on_drop*/ false); });
    }

    // Normal path: tag the container so Ryuk (and tooling) can find it
    // (managed-by + session), and auto-remove the handle on drop. Subject to
    // startup-attempt retry (each retry creates a brand-new container).
    for (const auto& label : detail::testcontainers_labels()) {
        spec.labels.push_back(label);
    }
    return with_retry([&] { return create_start_wait(spec, /*remove_on_drop*/ true); });
}

} // namespace detail

Container run(const ContainerRequest& request) {
    // Make sure the crash-safety reaper is up before we create anything it should
    // reap (no-op if Ryuk is disabled).
    detail::Reaper::instance().ensure_started();

    DockerClient client = DockerClient::from_environment();
    return detail::Runner::run(client, request);
}

Container run(DockerClient client, const ContainerRequest& request) {
    // Boot the reaper on the daemon the CALLER chose, not the environment one —
    // otherwise a remote-endpoint run would start Ryuk locally (or fail with no
    // local daemon) and the remote containers would never be watched.
    detail::Reaper::instance().ensure_started(client);
    return detail::Runner::run(client, request);
}

} // namespace testcontainers
