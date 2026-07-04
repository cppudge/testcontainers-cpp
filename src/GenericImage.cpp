#include "testcontainers/GenericImage.hpp"

#include <string>

#include "docker/ApiMapping.hpp"
#include "docker/Auth.hpp"
#include "testcontainers/Container.hpp"
#include "testcontainers/ContainerRequest.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"

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

ContainerRequest GenericImage::to_request() const {
    ContainerRequest request;
    request.spec = build_spec();
    request.exposed_ports = exposed_ports_;
    request.copy_to_sources = copy_to_sources_;
    request.waits = waits_;
    request.startup_timeout = startup_timeout_;
    request.registry_auth = registry_auth_;
    request.pull_policy = pull_policy_;
    request.reuse = reuse_;
    request.created_hooks = created_hooks_;
    request.starting_hooks = starting_hooks_;
    request.started_hooks = started_hooks_;
    request.stopping_hooks = stopping_hooks_;
    request.startup_attempts = startup_attempts_;
    return request;
}

Container GenericImage::start() const { return run(to_request()); }

} // namespace testcontainers
