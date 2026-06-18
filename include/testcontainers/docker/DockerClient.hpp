#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerHost.hpp"
#include "testcontainers/docker/Logs.hpp"

namespace testcontainers {

/// An HTTP response from the Docker Engine API.
struct Response {
    int status_code = 0;
    std::string reason;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    /// Case-insensitive header lookup; returns "" if absent.
    std::string header(std::string_view name) const;

    bool ok() const noexcept { return status_code >= 200 && status_code < 300; }
};

/// A synchronous client for the Docker Engine HTTP API.
///
/// Each request opens a fresh connection to the daemon over the resolved
/// transport (unix socket / Windows named pipe / TCP). Connection reuse and
/// streaming endpoints are added in later milestones.
class DockerClient {
public:
    /// Construct a client for an explicitly resolved host.
    explicit DockerClient(DockerHost host);

    /// Construct a client using DockerHost::resolve().
    static DockerClient from_environment();

    const DockerHost& host() const noexcept { return host_; }

    /// Perform an HTTP request against the daemon and return the full response.
    /// `target` is the path (e.g. "/_ping", "/v1.43/containers/json").
    Response request(std::string_view method, std::string_view target,
                     std::string_view body = {},
                     const std::vector<std::pair<std::string, std::string>>& headers = {});

    /// `GET /_ping` — true if the daemon answers with a 2xx status.
    bool ping();

    // --- Image operations ---

    /// `POST /images/create?fromImage=...` — pull an image (blocks until done).
    /// `image` is "name[:tag]" (tag defaults to "latest").
    void pull_image(const std::string& image);

    // --- Container lifecycle ---

    /// `POST /containers/create` — returns the new container id. If the image is
    /// missing (404), pulls it and retries once.
    std::string create_container(const CreateContainerSpec& spec);

    /// `POST /containers/{id}/start`.
    void start_container(const std::string& id);

    /// `GET /containers/{id}/json` — throws DockerError if the container is gone.
    ContainerInspect inspect_container(const std::string& id);

    /// `POST /containers/{id}/stop` (optional grace period in seconds).
    void stop_container(const std::string& id, std::optional<int> timeout_secs = std::nullopt);

    /// `DELETE /containers/{id}` — force-kill and remove anonymous volumes by default.
    void remove_container(const std::string& id, bool force = true, bool remove_volumes = true);

    /// `GET /containers/{id}/logs` — fetch a snapshot of the container's logs and
    /// demultiplex the (non-TTY) stream into separate stdout / stderr text.
    /// Only the non-follow case is supported here; `opts.follow` is ignored.
    ContainerLogs logs(const std::string& id, const LogOptions& opts = {});

private:
    DockerHost host_;
};

} // namespace testcontainers
