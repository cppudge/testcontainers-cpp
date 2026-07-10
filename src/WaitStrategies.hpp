#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    const std::chrono::steady_clock::time_point wake = now + value;
    return {wake < deadline ? wake : deadline, wake > deadline};
}

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
