#pragma once

#include <cstdint>
#include <map>
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

/// True if `TESTCONTAINERS_RYUK_DISABLED` is set to a truthy value ("1"/"true"),
/// else if ~/.testcontainers.properties sets `ryuk.disabled=true` (value
/// case-insensitive, java Boolean.parseBoolean parity).
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

/// Start a Ryuk container (image `testcontainers/ryuk`, overridable via env
/// TESTCONTAINERS_RYUK_CONTAINER_IMAGE / properties key `ryuk.container.image`;
/// docker.sock bind-mounted, 8080/tcp published) and return its endpoint,
/// WITHOUT registering any filter.
/// The caller owns the returned container (must remove it). Blocks until the
/// container is running and its control port has been resolved; does NOT wait
/// for the port to accept connections (the caller's connect-retry does that).
///
/// `auto_remove` sets HostConfig.AutoRemove so Docker deletes the Ryuk container
/// once it exits (after reaping). The per-daemon reaper passes true so it
/// leaves no stopped shell behind; callers that remove Ryuk themselves pass false.
RyukEndpoint start_ryuk(DockerClient& client, bool auto_remove = false);

/// The per-daemon resource-reaper registry (one instance per process).
///
/// The first `ensure_started()` against a daemon (unless Ryuk is disabled)
/// starts a Ryuk sidecar THERE, opens a persistent TCP connection to it, and
/// registers the session filter `label=org.testcontainers.session-id=<id>`.
/// Daemons are keyed by their endpoint URL (`DockerHost::to_string()`): clients
/// for the same endpoint share one reaper, and each further daemon used in the
/// process gets its own. Every connection is held open for the whole process;
/// when the process dies the OS closes them and each daemon's Ryuk (after its
/// reconnection timeout) reaps everything there carrying the session label.
class Reaper {
public:
    static Reaper& instance();

    Reaper(const Reaper&) = delete;
    Reaper& operator=(const Reaper&) = delete;

    /// Idempotent per daemon. No-op when Ryuk is disabled OR when the daemon is
    /// in Windows-containers mode (the Linux Ryuk image cannot run there, so
    /// there is no crash-safe reaping on Windows — matching
    /// testcontainers-dotnet). Otherwise starts that daemon's Ryuk and opens
    /// its persistent control connection exactly once. Throws DockerError on
    /// failure (so callers fail loudly rather than silently leaking) — a failed
    /// boot is retried on the next call. This overload targets the environment
    /// daemon (DockerClient::from_environment()).
    void ensure_started();

    /// As above, but for the daemon `client` points at — used by
    /// `run(DockerClient, ...)` so a caller-supplied endpoint gets its reaper
    /// on THAT daemon, not the environment one.
    void ensure_started(DockerClient& client);

    /// Register an ADDITIONAL reap filter (`label=<key>=<value>`) with the
    /// ENVIRONMENT daemon's Ryuk — for resources testcontainers cannot label
    /// itself (the compose CLI creates the project's containers/networks/
    /// volumes, which all carry `com.docker.compose.project=<project>`; compose
    /// always runs against the environment daemon). Boots that reaper first
    /// when needed; idempotent per (key, value). A no-op when Ryuk is disabled
    /// or was skipped (Windows engine). Throws DockerError when Ryuk does not
    /// acknowledge the filter — callers fail loudly rather than run without
    /// crash-safe reaping.
    void register_filter(const std::string& key, const std::string& value);

    /// The filter lines the ENVIRONMENT daemon's Ryuk has acknowledged, in
    /// registration order (the session filter first); empty when that reaper is
    /// disabled, skipped, or not yet started. A snapshot — exposed so tests can
    /// assert a filter really reached Ryuk.
    std::vector<std::string> registered_filters();

private:
    Reaper();
    ~Reaper();

    struct Impl;

    /// Find-or-boot the entry for `client`'s daemon. Assumes `mutex_` is held.
    /// First sight runs the bootstrap (Ryuk start + session filter + ACK) and
    /// records the entry; a null entry means "handled, but skipped" (Windows
    /// engine). Throws on a failed bootstrap WITHOUT recording anything, so the
    /// next call retries.
    Impl* daemon_locked(DockerClient& client);

    /// The actual bootstrap against `client`'s daemon. Assumes `mutex_` is
    /// held. Returns the connected per-daemon state, or null for the
    /// Windows-engine skip.
    std::unique_ptr<Impl> boot_daemon_locked(DockerClient& client);

    std::mutex mutex_;
    /// Endpoint URL -> reaper state (null = skipped). Entries live for the
    /// whole process; their destructor closes the control sockets, which is
    /// what lets each Ryuk reap after the process dies.
    std::map<std::string, std::unique_ptr<Impl>> daemons_;
};

} // namespace testcontainers::detail
