#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/ExecOptions.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/RegistryAuth.hpp"
#include "testcontainers/docker/BuildOptions.hpp"
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

    /// `GET /version` — the daemon's operating system (the `Os` field, e.g.
    /// "linux" / "windows"). Cached process-wide on first success: the engine
    /// mode (Linux vs Windows containers) does not change mid-process. Throws
    /// DockerError if the daemon is unreachable or the response is malformed.
    std::string server_os();

    /// True when the daemon is running in Windows-containers mode (server_os()
    /// contains "windows", case-insensitive). Used to skip the Linux-only Ryuk
    /// reaper and to route engine-specific tests.
    bool is_windows_engine();

    // --- Image operations ---

    /// `POST /images/create?fromImage=...` — pull an image (blocks until done).
    /// `image` is "name[:tag]" (tag defaults to "latest").
    ///
    /// When `auth` is provided it is sent verbatim as `X-Registry-Auth`;
    /// otherwise credentials are auto-resolved from the Docker config for the
    /// image's registry. A public pull (no credentials found) is unaffected.
    void pull_image(const std::string& image,
                    const std::optional<RegistryAuth>& auth = std::nullopt);

    /// `POST /build` — build an image from a tar build context (`context_tar`, an
    /// `application/x-tar` body). Blocks until the build finishes. Throws DockerError
    /// on a non-200 status or a build error embedded in the streamed output.
    void build_image(const std::string& context_tar, const docker::BuildOptions& options);

    // --- Container lifecycle ---

    /// `POST /containers/create` — returns the new container id. If the image is
    /// missing (404), pulls it (threading `auth` through) and retries once.
    std::string create_container(const CreateContainerSpec& spec,
                                 const std::optional<RegistryAuth>& auth = std::nullopt);

    /// `POST /containers/{id}/start`.
    void start_container(const std::string& id);

    /// `GET /containers/{id}/json` — throws DockerError if the container is gone.
    ContainerInspect inspect_container(const std::string& id);

    /// `GET /containers/json` filtered by label equality. `all` includes stopped
    /// containers. `label_filters` become Docker's filters={"label":["k=v",...]}.
    std::vector<ContainerSummary> list_containers(
        const std::vector<std::pair<std::string, std::string>>& label_filters, bool all = true);

    /// `POST /containers/{id}/stop` (optional grace period in seconds).
    void stop_container(const std::string& id, std::optional<int> timeout_secs = std::nullopt);

    /// `DELETE /containers/{id}` — force-kill and remove anonymous volumes by default.
    void remove_container(const std::string& id, bool force = true, bool remove_volumes = true);

    /// `GET /containers/{id}/logs` — fetch a snapshot of the container's logs and
    /// demultiplex the (non-TTY) stream into separate stdout / stderr text.
    /// Only the non-follow case is supported here; `opts.follow` is ignored.
    ContainerLogs logs(const std::string& id, const LogOptions& opts = {});

    /// `GET /containers/{id}/logs?follow=1` — stream the multiplexed logs, decoding
    /// frames and invoking `consumer` per chunk until the stream ends (container
    /// stops) or `consumer` returns false. Blocking: run on your own thread for
    /// background consumption. `opts.follow` is forced on. Throws DockerError if the
    /// initial response is not 200.
    void follow_logs(const std::string& id, const LogOptions& opts, const LogConsumer& consumer);

    /// Run `cmd` inside the running container and capture its output and exit
    /// code, using default options. Equivalent to the `opts` overload with a
    /// default-constructed `ExecOptions`.
    ExecResult exec(const std::string& id, const std::vector<std::string>& cmd);

    /// Run `cmd` inside the running container with `opts` (env / working dir /
    /// user / privileged / tty / stdin) and capture its output and exit code.
    /// Creates the exec (`POST /containers/{id}/exec`), starts it
    /// (`POST /exec/{exec_id}/start`) and reads the exit code
    /// (`GET /exec/{exec_id}/json`).
    ///
    /// With `opts.tty == false` the returned stream is the multiplexed frame
    /// format and is demuxed into `stdout_data` / `stderr_data`. With
    /// `opts.tty == true` the stream is raw and unframed: all of it goes to
    /// `stdout_data` and `stderr_data` is left empty. When `opts.stdin_data` is
    /// set those bytes are written to the exec's stdin and the send side is then
    /// half-closed so a reader (e.g. `cat`) sees EOF.
    ExecResult exec(const std::string& id, const std::vector<std::string>& cmd,
                    const ExecOptions& opts);

    /// Streaming variant of `exec`: starts `cmd` with `opts` and delivers output
    /// to `consumer` incrementally as the daemon flushes it (blocking until the
    /// command finishes or `consumer` returns false). With `opts.tty == false` the
    /// stream is demuxed and each chunk is reported with its `LogSource`; with
    /// `opts.tty == true` the raw stream is reported as `LogSource::Stdout`.
    ///
    /// Returns an `ExecResult` whose `exit_code` is read from the exec inspect;
    /// `stdout_data` / `stderr_data` are left empty (the output was delivered to
    /// `consumer`).
    ExecResult exec(const std::string& id, const std::vector<std::string>& cmd,
                    const ExecOptions& opts, const LogConsumer& consumer);

    /// `PUT /containers/{id}/archive?path=/` — copy a host file or in-memory
    /// bytes into the container by extracting a single-entry tar at the root.
    /// The entry name is the target with its leading '/' stripped, so the
    /// target's parent directory must already exist in the container. Throws
    /// DockerError on failure (non-200, or the host file cannot be read).
    void copy_to_container(const std::string& id, const CopyToContainer& source);

    /// `GET /containers/{id}/archive?path=<container_path>` — fetch the tar archive
    /// of the file or directory at `container_path`. Returns the raw tar bytes
    /// (extract with docker::extract_tar). Throws DockerError on 404 (no such
    /// container or path) or any non-200.
    std::string copy_from_container(const std::string& id, const std::string& container_path);

    // --- Network operations ---

    /// `POST /networks/create` — create a user-defined network, returning its id.
    /// `labels` are emitted as the network's `Labels` map (e.g. for Ryuk reaping).
    std::string create_network(const std::string& name,
                               const std::vector<std::pair<std::string, std::string>>& labels = {});

    /// `POST /networks/create` from a full spec; returns the new network id.
    std::string create_network(const NetworkCreateSpec& spec);

    /// `POST /networks/{id}/connect` — attach an existing container, optionally with
    /// DNS aliases on this network.
    void connect_network(const std::string& network_id, const std::string& container_id,
                         const std::vector<std::string>& aliases = {});

    /// `DELETE /networks/{id}` — remove a network (204 expected).
    void remove_network(const std::string& id);

private:
    DockerHost host_;
};

} // namespace testcontainers
