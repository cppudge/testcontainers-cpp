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

Container GenericImage::start() const {
    // Make sure the crash-safety reaper is up before we create anything it should
    // reap (no-op if Ryuk is disabled).
    detail::Reaper::instance().ensure_started();

    DockerClient client = DockerClient::from_environment();

    CreateContainerSpec spec;
    // Resolve the effective image reference: a custom substitutor overrides the
    // default env-prefix substitution (TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX).
    const std::string raw_ref = image_ + ":" + tag_;
    const std::string effective =
        substitutor_ ? substitutor_(raw_ref) : docker::substitute_image_name(raw_ref);
    spec.image = effective;
    spec.cmd = cmd_;
    spec.entrypoint = entrypoint_;
    spec.working_dir = working_dir_;
    spec.user = user_;
    spec.privileged = privileged_;
    spec.tty = tty_;
    spec.mounts = mounts_;
    spec.memory_bytes = memory_bytes_;
    spec.shm_size_bytes = shm_size_bytes_;
    spec.ulimits = ulimits_;
    spec.cap_add = cap_add_;
    spec.cap_drop = cap_drop_;
    spec.extra_hosts = extra_hosts_;
    spec.create_body_patch = create_body_patch_;
    spec.labels = labels_;
    for (const auto& [key, value] : env_) {
        spec.env.push_back(key + "=" + value);
    }
    for (const ContainerPort& p : exposed_ports_) {
        spec.exposed_ports.push_back(to_string(p));
    }
    // Let Docker assign host ports for everything we expose.
    spec.publish_all_ports = !exposed_ports_.empty();
    spec.healthcheck = healthcheck_;
    spec.network = network_;
    spec.network_aliases = network_aliases_;
    spec.name = container_name_;
    spec.platform = platform_;

    // ImagePullPolicy::Always pulls before create even when the image is present
    // locally; Default relies on create's lazy pull-on-404 path.
    if (pull_policy_ == ImagePullPolicy::Always) {
        client.pull_image(effective, registry_auth_);
    }

    // The shared create→copy→start→wait tail, returning a handle. `remove_on_drop`
    // is false for reusable containers (they must persist across runs).
    const auto create_start_wait = [&](const CreateContainerSpec& s,
                                       bool remove_on_drop) -> Container {
        const std::string id = client.create_container(s, registry_auth_);

        // Copy files/data in after create, before start (the create→copy→start
        // order). A copy failure must not leak the partially-created container.
        try {
            for (const CopyToContainer& source : copy_to_sources_) {
                client.copy_to_container(id, source);
            }
        } catch (...) {
            try {
                client.remove_container(id, /*force*/ true, /*remove_volumes*/ true);
            } catch (...) {
            }
            throw;
        }

        client.start_container(id);

        try {
            detail::wait_until_ready(client, id, waits_, startup_timeout_, tty_);
        } catch (...) {
            // A container that started but never became ready must not leak.
            try {
                client.remove_container(id, /*force*/ true, /*remove_volumes*/ true);
            } catch (...) {
            }
            throw;
        }

        return Container(std::move(client), id, remove_on_drop, tty_);
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
            canonical += "\n" + s.target() + "\n" + (s.is_file() ? s.host_path() : s.bytes());
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
                // Adopt it: wait for readiness, return a NON-removing handle.
                detail::wait_until_ready(client, m.id, waits_, startup_timeout_, tty_);
                return Container(std::move(client), m.id, /*remove_on_drop*/ false, tty_);
            }
        }
        // No match: create a NEW reuse container (persistent, not reaped).
        return create_start_wait(spec, /*remove_on_drop*/ false);
    }

    // Normal path: tag the container so Ryuk (and tooling) can find it
    // (managed-by + session), and auto-remove the handle on drop.
    for (const auto& label : detail::testcontainers_labels()) {
        spec.labels.push_back(label);
    }
    return create_start_wait(spec, /*remove_on_drop*/ true);
}

} // namespace testcontainers
