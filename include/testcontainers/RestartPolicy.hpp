#pragma once

#include <string>

namespace testcontainers {

/// A daemon-side restart policy (`HostConfig.RestartPolicy`): when the
/// container exits, the daemon itself restarts it. Build one via the named
/// factories, e.g. `RestartPolicy::on_failure(3)`.
///
/// Two interactions worth knowing in test code: the daemon rejects a restart
/// policy combined with auto-remove, and a restarting container is still
/// Ryuk-reaped at process exit — `always()` keeps a crashing service alive
/// DURING the test session, it does not outlive it (combine with reuse for a
/// container that survives across runs).
struct RestartPolicy {
    /// Docker's `Name` values: "no", "always", "unless-stopped", "on-failure".
    std::string name;
    /// Give-up threshold after consecutive failed restarts; only valid
    /// nonzero with "on-failure" (0 = keep retrying without a cap).
    int maximum_retry_count = 0;

    /// Restart whenever the container exits, regardless of exit code.
    static RestartPolicy always() { return {"always", 0}; }
    /// Like `always()`, except an explicit stop (docker stop) sticks.
    static RestartPolicy unless_stopped() { return {"unless-stopped", 0}; }
    /// Restart only after a nonzero exit, giving up after `max_retries`
    /// consecutive failures (0 = no cap).
    static RestartPolicy on_failure(int max_retries) { return {"on-failure", max_retries}; }
};

} // namespace testcontainers
