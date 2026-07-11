#include "testcontainers/GenericImage.hpp"

#include <string>

#include "docker/ApiMapping.hpp"
#include "docker/Auth.hpp"
#include "testcontainers/Container.hpp"
#include "testcontainers/ContainerRequest.hpp"
#include "testcontainers/Network.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers {

GenericImage GenericImage::from_reference(const std::string& reference) {
    const auto [image, tag] = docker::split_image(reference);
    return GenericImage(image, tag);
}

GenericImage& GenericImage::with_image(const std::string& reference) {
    auto [image, tag] = docker::split_image(reference);
    image_ = std::move(image);
    tag_ = std::move(tag);
    return *this;
}

bool GenericImage::exists(const std::string& name, const std::string& tag) {
    DockerClient client = DockerClient::from_environment();
    return client.image_exists(name + ":" + tag);
}

ImageInspect GenericImage::inspect(const std::string& name, const std::string& tag) {
    return DockerClient::from_environment().inspect_image(name + ":" + tag);
}

ImageInspect GenericImage::inspect() const { return inspect(image_, tag_); }

// Out of line so the header only needs a forward declaration of Network.
GenericImage& GenericImage::with_network(const Network& network) {
    return with_network(network.name());
}

ContainerRequest GenericImage::to_request() const {
    // The embedded request already carries everything except the two fields
    // translated lazily: the image reference and env. Patch them into a COPY —
    // the builder stays reusable and repeated snapshots must not accumulate.
    ContainerRequest request = request_;

    // Resolve the effective image reference: a custom substitutor overrides the
    // default env-prefix substitution (TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX).
    const std::string raw_ref = image_ + ":" + tag_;
    request.spec.image =
        substitutor_ ? substitutor_(raw_ref) : docker::substitute_image_name(raw_ref);

    for (const auto& [key, value] : env_) {
        // Build in place: chained + would allocate a temporary per entry.
        std::string& entry = request.spec.env.emplace_back(key);
        entry += '=';
        entry += value;
    }
    return request;
}

Container GenericImage::start() const { return run(to_request()); }

} // namespace testcontainers
