#pragma once

#include <string>

#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers {

/// A RAII handle to a user-defined Docker network.
///
/// Move-only: it owns a real external resource and removes the network on
/// destruction (best-effort, exceptions swallowed). Copying is deleted so the
/// removal happens exactly once. Containers placed on the same network (via
/// `GenericImage::with_network`) can resolve each other by container name.
class Network {
public:
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    Network(Network&& other) noexcept
        : client_(std::move(other.client_)), id_(std::move(other.id_)),
          name_(std::move(other.name_)), dropped_(other.dropped_) {
        other.dropped_ = true; // the moved-from handle owns nothing
    }

    Network& operator=(Network&& other) noexcept {
        if (this != &other) {
            drop();
            client_ = std::move(other.client_);
            id_ = std::move(other.id_);
            name_ = std::move(other.name_);
            dropped_ = other.dropped_;
            other.dropped_ = true;
        }
        return *this;
    }

    /// Removes the network unless it was already explicitly removed or
    /// moved-from. Never throws.
    ~Network() { drop(); }

    /// Create a network with the given name.
    static Network create(std::string name);

    /// Create a network with a generated unique name (`tc-<random hex>`).
    static Network create();

    /// The network's name (used by containers to join it / resolve peers).
    const std::string& name() const noexcept { return name_; }

    /// The network's id.
    const std::string& id() const noexcept { return id_; }

    /// Explicitly remove the network now. Idempotent; after this the destructor
    /// does nothing.
    void remove();

private:
    Network(DockerClient client, std::string id, std::string name)
        : client_(std::move(client)), id_(std::move(id)), name_(std::move(name)) {}

    /// Best-effort remove, swallowing any error. Marks the handle dropped.
    void drop() noexcept;

    // Mutable for the same reason as Container: the client is a stateless host
    // config that opens a fresh connection per call.
    mutable DockerClient client_;
    std::string id_;
    std::string name_;
    bool dropped_ = false;
};

} // namespace testcontainers
