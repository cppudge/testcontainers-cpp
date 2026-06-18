#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/Logs.hpp"

namespace testcontainers {

/// A RAII handle to a running container.
///
/// Move-only: it owns a real external resource and force-removes the container
/// on destruction (best-effort, exceptions swallowed). Copying is deleted so the
/// removal happens exactly once.
class Container {
public:
    /// Adopt an already-created, already-started container. Used by
    /// `GenericImage::start()`.
    Container(DockerClient client, std::string id)
        : client_(std::move(client)), id_(std::move(id)) {}

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    Container(Container&& other) noexcept
        : client_(std::move(other.client_)), id_(std::move(other.id_)),
          dropped_(other.dropped_) {
        other.dropped_ = true; // the moved-from handle owns nothing
    }

    Container& operator=(Container&& other) noexcept {
        if (this != &other) {
            drop();
            client_ = std::move(other.client_);
            id_ = std::move(other.id_);
            dropped_ = other.dropped_;
            other.dropped_ = true;
        }
        return *this;
    }

    /// Force-removes the container unless it was already explicitly removed or
    /// moved-from. Never throws.
    ~Container() { drop(); }

    /// The full container id.
    const std::string& id() const noexcept { return id_; }

    /// The address a client on this host should connect to ("localhost" for a
    /// unix socket / named pipe, otherwise the daemon hostname).
    std::string host() const { return client_.host().http_host(); }

    /// The host port Docker published for the given container port. Throws
    /// DockerError if the port is not exposed/published.
    std::uint16_t get_host_port(ContainerPort port) const;

    /// A snapshot of the container's stdout / stderr logs.
    ContainerLogs logs() const;

    /// Run a command inside the running container, capturing its stdout / stderr
    /// and exit code.
    ExecResult exec(const std::vector<std::string>& cmd) const;

    /// Stop the container (it is still removed on destruction).
    void stop();

    /// Whether the container is currently running (per a fresh inspect).
    bool is_running() const;

    /// Explicitly stop owning / force-remove the container now. Idempotent;
    /// after this the destructor does nothing.
    void remove();

private:
    /// Best-effort force-remove, swallowing any error. Marks the handle dropped.
    void drop() noexcept;

    // Mutable: the client is just a stateless host config that opens a fresh
    // connection per call, so issuing requests through it is logically const
    // from the handle's point of view (const accessors like get_host_port /
    // is_running / logs need it).
    mutable DockerClient client_;
    std::string id_;
    bool dropped_ = false;
};

} // namespace testcontainers
