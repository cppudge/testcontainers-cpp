#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Deadline.hpp"
#include "testcontainers/WaitFor.hpp"

namespace testcontainers {

class DockerClient;

namespace detail {

/// Count non-overlapping occurrences of `needle` in `haystack` (0 for an empty
/// needle). Drives the log wait's "seen the message N times" check. Pure —
/// exposed for unit testing.
std::size_t count_occurrences(const std::string& haystack, const std::string& needle);

/// Streaming sibling of count_occurrences: feed the log text chunk by chunk
/// (at arbitrary split points) and count non-overlapping occurrences of
/// `needle` across the whole sequence — a match spanning a chunk boundary
/// still counts. Memory stays bounded by the unmatched tail: the consumed
/// prefix is trimmed once it grows large, so a chatty container does not
/// accumulate its entire log. Empty needle: counts nothing (parity with
/// count_occurrences). Exposed for unit testing.
class OccurrenceCounter {
public:
    explicit OccurrenceCounter(std::string needle) : needle_(std::move(needle)) {}

    void feed(std::string_view chunk);

    std::size_t count() const noexcept { return count_; }

private:
    std::string needle_;
    std::string buffered_;      ///< unscanned tail (plus a trimmed-lazily prefix)
    std::size_t scan_from_ = 0; ///< first byte a future match may start at
    std::size_t count_ = 0;
};

/// The sleep plan for a `wait_for::Duration` under the shared readiness
/// deadline: sleep until `wake` (the requested duration clamped to the
/// deadline); `times_out` says the requested duration did not fit the
/// remaining budget, i.e. the caller must throw StartupTimeoutError after
/// the (clamped) sleep. A duration ending EXACTLY on the deadline still
/// fits — the budget is spent, not overspent. Pure — exposed for unit
/// testing.
struct ClampedWaitPlan {
    std::chrono::steady_clock::time_point wake;
    bool times_out = false;
};

inline ClampedWaitPlan clamped_wait_plan(std::chrono::steady_clock::time_point now,
                                         std::chrono::milliseconds value,
                                         std::chrono::steady_clock::time_point deadline) {
    // saturated_add, not '+': a milliseconds::max()-sized duration must clamp
    // to the far future (and so time out), not overflow into the past.
    const std::chrono::steady_clock::time_point wake = detail::saturated_add(now, value);
    return {wake < deadline ? wake : deadline, wake > deadline};
}

/// Per-probe deadline: the time left until `deadline`, capped at 5s and
/// floored at 1ms. A port that ACCEPTS the connection but never answers is an
/// ordinary startup state (listener up, application not serving yet) — an
/// unbounded probe there would hang the whole wait past its own deadline.
///
/// The cap must absorb Windows' refused-connect retry: "localhost" resolves to
/// [::1, 127.0.0.1] and Docker Desktop's proxy listens on IPv4 only, so every
/// probe first burns ~2s on the ::1 attempt (WinSock retries a refused
/// loopback SYN) before 127.0.0.1 succeeds — a 2s cap made every probe time
/// out mid-range-connect and the wait never became ready.
std::chrono::milliseconds probe_budget(std::chrono::steady_clock::time_point deadline);

/// One TCP probe, bounded by `budget`: resolve host:port and open a
/// connection. Returns true on a successful connect (the caller treats that as
/// "ready"), false on any refusal/unreachable/timeout (treated as "not ready
/// yet"). Shared by the Port wait strategy and the compose published-port
/// wait.
bool tcp_probe(const std::string& host, std::uint16_t port, std::chrono::milliseconds budget);

/// One HTTP probe, bounded by `budget`: open a TCP connection to host:port,
/// GET `path`, and return the response status — or std::nullopt when the
/// connection/exchange failed, timed out, or the peer closed without sending
/// a single response byte (the caller treats all of those as "not ready
/// yet"). The zero-byte-close case is load-bearing: Docker Desktop's port
/// proxy accepts connections for a published port and, while the container
/// backend is not listening yet, closes them gracefully after the request —
/// which must read as "not ready", never as a status. Drives the Http wait
/// strategy; exposed for unit testing.
std::optional<int> http_probe(const std::string& host, std::uint16_t port, const std::string& path,
                              std::chrono::milliseconds budget);

/// Run each readiness condition in `waits` in order, under a single shared
/// deadline (`timeout` from the moment this is called). Throws
/// StartupTimeoutError if the deadline passes before a condition is met (and
/// DockerError for non-timeout failures, e.g. a wrong exit code or a container
/// with no healthcheck). `tty` records whether the
/// container was created with Tty=true, so log-based waits read its raw/unframed
/// stream instead of demuxing (which would garble it).
///
/// Inspect-based conditions poll every ~200ms over one kept-alive connection;
/// the log condition streams the log (deadline-bounded follow) instead.
void wait_until_ready(DockerClient& client, const std::string& id,
                      const std::vector<WaitFor>& waits, std::chrono::milliseconds timeout,
                      bool tty = false);

} // namespace detail
} // namespace testcontainers
