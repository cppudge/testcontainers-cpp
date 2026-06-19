#include "testcontainers/GenericImage.hpp"

#include <string>
#include <utility>

#include "Reaper.hpp"
#include "WaitStrategies.hpp"
#include "docker/ApiMapping.hpp"
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
    spec.image = image_ + ":" + tag_;
    spec.cmd = cmd_;
    spec.entrypoint = entrypoint_;
    spec.working_dir = working_dir_;
    spec.user = user_;
    spec.privileged = privileged_;
    spec.mounts = mounts_;
    spec.memory_bytes = memory_bytes_;
    spec.shm_size_bytes = shm_size_bytes_;
    spec.ulimits = ulimits_;
    spec.cap_add = cap_add_;
    spec.cap_drop = cap_drop_;
    spec.extra_hosts = extra_hosts_;
    spec.create_body_patch = create_body_patch_;
    spec.labels = labels_;
    // Tag the container so Ryuk (and tooling) can find it: managed-by + session.
    for (const auto& label : detail::testcontainers_labels()) {
        spec.labels.push_back(label);
    }
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
    spec.name = container_name_;
    spec.platform = platform_;

    const std::string id = client.create_container(spec, registry_auth_);

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
        detail::wait_until_ready(client, id, waits_, startup_timeout_);
    } catch (...) {
        // A container that started but never became ready must not leak.
        try {
            client.remove_container(id, /*force*/ true, /*remove_volumes*/ true);
        } catch (...) {
        }
        throw;
    }

    return Container(std::move(client), id);
}

} // namespace testcontainers
