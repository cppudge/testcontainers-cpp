#include "testcontainers/GenericImage.hpp"

#include <string>
#include <utility>

#include "WaitStrategies.hpp"
#include "testcontainers/Container.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers {

Container GenericImage::start() const {
    DockerClient client = DockerClient::from_environment();

    CreateContainerSpec spec;
    spec.image = image_ + ":" + tag_;
    spec.cmd = cmd_;
    spec.entrypoint = entrypoint_;
    spec.working_dir = working_dir_;
    spec.user = user_;
    spec.privileged = privileged_;
    spec.mounts = mounts_;
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

    const std::string id = client.create_container(spec);
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
