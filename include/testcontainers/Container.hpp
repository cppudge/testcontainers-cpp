#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/ExecOptions.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/Logs.hpp"

namespace testcontainers {

/// A RAII handle to a running container.
///
/// Move-only: it owns a real external resource and force-removes the container
/// on destruction (best-effort, exceptions swallowed). Copying is deleted so the
/// removal happens exactly once.
///
/// A persistent handle (`remove_on_drop == false`) does NOT remove the container
/// on destruction; it is used for reusable containers (`with_reuse`), which must
/// survive across test runs. The caller is then responsible for removing it.
class Container {
public:
    /// Adopt an already-created, already-started container. Used by
    /// `GenericImage::start()`. With `remove_on_drop == false` the handle is
    /// persistent: it never removes the container (reusable containers).
    Container(DockerClient client, std::string id, bool remove_on_drop = true)
        : client_(std::move(client)), id_(std::move(id)), remove_on_drop_(remove_on_drop) {}

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    Container(Container&& other) noexcept
        : client_(std::move(other.client_)), id_(std::move(other.id_)),
          dropped_(other.dropped_), remove_on_drop_(other.remove_on_drop_) {
        other.dropped_ = true; // the moved-from handle owns nothing
    }

    Container& operator=(Container&& other) noexcept {
        if (this != &other) {
            drop();
            client_ = std::move(other.client_);
            id_ = std::move(other.id_);
            dropped_ = other.dropped_;
            remove_on_drop_ = other.remove_on_drop_;
            other.dropped_ = true;
        }
        return *this;
    }

    /// Force-removes the container unless it was already explicitly removed or
    /// moved-from. Never throws.
    ~Container() { drop(); }

    /// The full container id.
    const std::string& id() const noexcept { return id_; }

    /// True when this is a persistent (reusable) handle that will NOT remove the
    /// container on destruction.
    bool is_persistent() const noexcept { return !remove_on_drop_; }

    /// The address a client on this host should connect to ("localhost" for a
    /// unix socket / named pipe, otherwise the daemon hostname).
    std::string host() const { return client_.host().http_host(); }

    /// The host port Docker published for the given container port. Throws
    /// DockerError if the port is not exposed/published.
    std::uint16_t get_host_port(ContainerPort port) const;

    /// A snapshot of the container's stdout / stderr logs.
    ContainerLogs logs() const;

    /// Stream this container's logs to `consumer` until the container stops or the
    /// consumer returns false. Blocking — run on your own std::thread for background
    /// consumption. See DockerClient::follow_logs.
    void follow_logs(const LogConsumer& consumer, const LogOptions& opts = {}) const;

    /// Run a command inside the running container, capturing its stdout / stderr
    /// and exit code.
    ExecResult exec(const std::vector<std::string>& cmd) const;

    /// Run a command inside the running container with `opts` (env / working dir /
    /// user / privileged / tty / stdin), capturing its output and exit code. See
    /// DockerClient::exec for the tty/stdin semantics.
    ExecResult exec(const std::vector<std::string>& cmd, const ExecOptions& opts) const;

    /// Streaming variant: run `cmd` with `opts`, delivering output to `consumer`
    /// incrementally and returning an ExecResult with the exit code set (the
    /// stdout/stderr fields are left empty — the output went to `consumer`). See
    /// DockerClient::exec.
    ExecResult exec(const std::vector<std::string>& cmd, const ExecOptions& opts,
                    const LogConsumer& consumer) const;

    /// Copy a host file or in-memory bytes into this already-running container
    /// (`PUT /containers/{id}/archive`). The target's parent directory must
    /// already exist. Throws DockerError on failure.
    void copy_to(const CopyToContainer& source) const;

    /// Read a single regular file out of the container and return its bytes.
    /// Fetches `GET .../archive` for `container_path` and extracts the one regular
    /// file in the archive. Throws DockerError if the path is not a single regular
    /// file (e.g. a directory). The bytes may be binary.
    std::string read_file(const std::string& container_path) const;

    /// Copy a single regular file out of the container to `host_dest` (a host file
    /// path; its parent directory is created if missing). Throws DockerError on
    /// failure. For directory trees use copy_from_container + extract_tar directly.
    void copy_file_from(const std::string& container_path, const std::string& host_dest) const;

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
    bool remove_on_drop_ = true; ///< false for persistent (reusable) handles
};

} // namespace testcontainers
