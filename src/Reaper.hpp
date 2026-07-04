#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace testcontainers {
class DockerClient;
}

namespace testcontainers::detail {

/// Process-wide session id, generated once on first use (random hex). Every
/// resource testcontainers creates is tagged with this so a single Ryuk reaper
/// can reap them all when the owning process dies.
const std::string& session_id();

/// The labels every testcontainers-managed resource carries:
///   org.testcontainers.managed-by = testcontainers
///   org.testcontainers.session-id = <session_id()>
/// When Ryuk is disabled, the session-id label is omitted (no reaper uses it).
std::vector<std::pair<std::string, std::string>> testcontainers_labels();

/// True if `TESTCONTAINERS_RYUK_DISABLED` is set to a truthy value ("1"/"true").
bool ryuk_disabled();

/// The newline-terminated filter line Ryuk expects on its control socket:
///   "label=<key>=<value>\n"
/// Pure helper, unit-testable without a daemon.
std::string ryuk_filter_line(const std::string& key, const std::string& value);

/// A started (but unregistered) Ryuk container and where to reach its control
/// port. Returned by `start_ryuk` so tests can drive a dedicated reaper.
struct RyukEndpoint {
    std::string container_id;
    std::string host;
    std::uint16_t port = 0;
};

/// Start a Ryuk container (image `testcontainers/ryuk`, docker.sock bind-mounted,
/// 8080/tcp published) and return its endpoint, WITHOUT registering any filter.
/// The caller owns the returned container (must remove it). Blocks until the
/// container is running and its control port has been resolved; does NOT wait
/// for the port to accept connections (the caller's connect-retry does that).
///
/// `auto_remove` sets HostConfig.AutoRemove so Docker deletes the Ryuk container
/// once it exits (after reaping). The process-global reaper passes true so it
/// leaves no stopped shell behind; callers that remove Ryuk themselves pass false.
RyukEndpoint start_ryuk(DockerClient& client, bool auto_remove = false);

/// The process-global resource reaper.
///
/// On first `ensure_started()` (unless Ryuk is disabled) it starts a single Ryuk
/// sidecar, opens a persistent TCP connection to it, and registers the session
/// filter `label=org.testcontainers.session-id=<id>`. The connection is held
/// open for the whole process; when the process dies the OS closes it and Ryuk
/// (after its reconnection timeout) reaps everything carrying the session label.
class Reaper {
public:
    static Reaper& instance();

    Reaper(const Reaper&) = delete;
    Reaper& operator=(const Reaper&) = delete;

    /// Idempotent. No-op when Ryuk is disabled OR when the daemon is in
    /// Windows-containers mode (the Linux Ryuk image cannot run there, so there
    /// is no crash-safe reaping on Windows — matching testcontainers-dotnet).
    /// Otherwise starts Ryuk and opens the persistent control connection exactly
    /// once. Throws DockerError on failure (so callers fail loudly rather than
    /// silently leaking). This overload targets the environment daemon
    /// (DockerClient::from_environment()).
    void ensure_started();

    /// As above, but starts Ryuk on the daemon `client` points at — used by
    /// `run(DockerClient, ...)` so a caller-supplied endpoint gets its reaper on
    /// THAT daemon, not the environment one. Singleton residual: the process-
    /// global reaper binds to the FIRST daemon it is started against; later
    /// calls against a different daemon are no-ops (labels applied, no reaping).
    void ensure_started(DockerClient& client);

private:
    Reaper();
    ~Reaper();

    /// The actual bootstrap (Ryuk start + filter registration + ACK) against
    /// `client`'s daemon. Assumes `mutex_` is held and `started_` is false;
    /// sets `started_` on success (including the skip paths).
    void start_locked(DockerClient& client);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::mutex mutex_;
    bool started_ = false;
};

} // namespace testcontainers::detail
