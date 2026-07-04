#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers::detail {

/// The hostname containers use to reach services listening on the test-process
/// host (see `GenericImage::with_exposed_host_port`).
inline constexpr const char* kHostAccessAlias = "host.testcontainers.internal";

/// The process-global host-port-exposure machinery.
///
/// Containers cannot in general reach the machine the tests run on (the
/// daemon may be remote, and `host-gateway` support varies by engine), so the
/// standard Testcontainers trick is used instead: a `testcontainers/sshd`
/// sidecar is started on the daemon, an SSH connection is opened to it from
/// the test process, and each exposed host port becomes an SSH REMOTE port
/// forward — a connection made inside the container network to
/// `<sidecar>:port` travels back through the SSH connection and is delivered
/// to `127.0.0.1:port` in the test process's network namespace.
///
/// One sidecar + one SSH session serve the whole process; `wire()` is called
/// by the start orchestration for every request carrying host-access ports.
class HostPortForwarder {
public:
    static HostPortForwarder& instance();

    HostPortForwarder(const HostPortForwarder&) = delete;
    HostPortForwarder& operator=(const HostPortForwarder&) = delete;

    /// Make `ports` on the test-process host reachable from the container
    /// `spec` describes, and point `kHostAccessAlias` at the sidecar by
    /// appending an ExtraHosts entry to `spec`. Starts the sidecar and the
    /// SSH tunnel on first use (on `client`'s daemon); ensures a remote
    /// forward exists per port; joins the sidecar to `spec`'s user-defined
    /// network when it has one. Thread-safe. Throws DockerError on a
    /// Windows-containers daemon, an unsupported network mode ("host",
    /// "none", "container:..."), or any sidecar/SSH failure.
    void wire(DockerClient& client, CreateContainerSpec& spec,
              const std::vector<std::uint16_t>& ports);

    /// Detach the sidecar from `network` (name or id) if `wire()` joined it —
    /// otherwise removing the network would fail with "active endpoints".
    /// Called by Network teardown; best-effort and never throws. If another
    /// live container still uses host access on that network, its alias stops
    /// resolving — but removing the network would have broken it anyway.
    void release_network(DockerClient& client, const std::string& network) noexcept;

private:
    HostPortForwarder() = default;
    ~HostPortForwarder();

    struct State;

    /// Start the sshd sidecar + SSH tunnel on `client`'s daemon.
    std::unique_ptr<State> make_state(DockerClient& client);
    std::mutex mutex_;
    std::unique_ptr<State> state_; ///< created on first wire(); guarded by mutex_
};

} // namespace testcontainers::detail
