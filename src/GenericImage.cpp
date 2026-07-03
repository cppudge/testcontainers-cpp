#include "testcontainers/GenericImage.hpp"

#include <string>
#include <utility>

#include "Reaper.hpp"
#include "Reuse.hpp"
#include "WaitStrategies.hpp"
#include "docker/ApiMapping.hpp"
#include "docker/Auth.hpp"
#include "testcontainers/Container.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers {

GenericImage GenericImage::from_reference(const std::string& reference) {
    const auto [image, tag] = docker::split_image(reference);
    return GenericImage(image, tag);
}

CreateContainerSpec GenericImage::build_spec() const {
    // Start from the embedded spec — it already carries every verbatim create
    // field (cmd, mounts, labels, host-config knobs, network, name, platform, …).
    CreateContainerSpec spec = spec_;

    // Resolve the effective image reference: a custom substitutor overrides the
    // default env-prefix substitution (TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX).
    const std::string raw_ref = image_ + ":" + tag_;
    spec.image = substitutor_ ? substitutor_(raw_ref) : docker::substitute_image_name(raw_ref);

    for (const auto& [key, value] : env_) {
        spec.env.push_back(key + "=" + value);
    }
    for (const ContainerPort& p : exposed_ports_) {
        spec.exposed_ports.push_back(to_string(p));
    }
    // Let Docker assign host ports for everything we expose.
    spec.publish_all_ports = !exposed_ports_.empty();

    return spec;
}

Container GenericImage::start() const {
    // Make sure the crash-safety reaper is up before we create anything it should
    // reap (no-op if Ryuk is disabled).
    detail::Reaper::instance().ensure_started();

    DockerClient client = DockerClient::from_environment();

    // Assemble the create spec (verbatim fields + the translated image/env/ports).
    CreateContainerSpec spec = build_spec();

    // ImagePullPolicy::Always pulls before create even when the image is present
    // locally; Default relies on create's lazy pull-on-404 path.
    if (pull_policy_ == ImagePullPolicy::Always) {
        client.pull_image(spec.image, registry_auth_);
    }

    // The shared create→copy→start→wait tail, returning a handle. `remove_on_drop`
    // is false for reusable containers (they must persist across runs). A throwing
    // created/starting/started hook is treated like any other failure here: it is
    // inside the try so the partial container is best-effort removed before the
    // exception propagates (no leak at any of the three points).
    const auto create_start_wait = [&](const CreateContainerSpec& s,
                                       bool remove_on_drop) -> Container {
        const std::string id = client.create_container(s, registry_auth_);

        // Everything from here until the container is constructed must, on any
        // throw, best-effort remove the partially-created container so it never
        // leaks — including a throwing created/starting/started hook.
        try {
            // created hooks: container exists (id assigned), before copy-to/start.
            for (const LifecycleHook& hook : created_hooks_) {
                if (hook) {
                    hook(client, id);
                }
            }

            // Copy files/data in after create, before start (the create→copy→start
            // order).
            for (const CopyToContainer& source : copy_to_sources_) {
                client.copy_to_container(id, source);
            }

            // starting hooks: after copy-to, immediately before start.
            for (const LifecycleHook& hook : starting_hooks_) {
                if (hook) {
                    hook(client, id);
                }
            }

            client.start_container(id);

            detail::wait_until_ready(client, id, waits_, startup_timeout_, spec.tty);

            // started hooks: after wait-until-ready, before constructing the handle.
            for (const LifecycleHook& hook : started_hooks_) {
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

        // The handle gets its own client copy so the captured `client` stays
        // usable across startup-attempt retries (DockerClient is a stateless host
        // config — opening a fresh connection per call).
        Container c(client, id, remove_on_drop, spec.tty);
        c.set_stopping_hooks(stopping_hooks_);
        c.set_exposed_ports(exposed_ports_);
        return c;
    };

    // Run an attempt-producing factory up to startup_attempts_ times: on a thrown
    // failure, if attempts remain, swallow and retry (each retry builds a brand-new
    // container via the factory); after the final attempt, rethrow the last error.
    const auto with_retry = [&](const auto& attempt) -> Container {
        const int attempts = std::max(1, startup_attempts_);
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
    // with_reuse(true) degrades to a normal (reaped, auto-removed) container.
    const bool use_reuse = reuse_ && detail::reuse_enabled();
    if (use_reuse) {
        // Canonical config for the hash: the create body WITHOUT any reuse/session
        // labels, plus the copy-to descriptors (so copied content participates).
        std::string canonical = docker::build_create_body(spec).dump();
        for (const CopyToContainer& s : copy_to_sources_) {
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
                detail::wait_until_ready(client, m.id, waits_, startup_timeout_, spec.tty);
                Container c(std::move(client), m.id, /*remove_on_drop*/ false, spec.tty);
                c.set_stopping_hooks(stopping_hooks_);
                c.set_exposed_ports(exposed_ports_);
                return c;
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

} // namespace testcontainers
