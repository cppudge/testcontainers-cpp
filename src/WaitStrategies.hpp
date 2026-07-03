#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "testcontainers/WaitFor.hpp"

namespace testcontainers {

class DockerClient;

namespace detail {

/// Count non-overlapping occurrences of `needle` in `haystack` (0 for an empty
/// needle). Drives the log wait's "seen the message N times" check. Pure —
/// exposed for unit testing.
std::size_t count_occurrences(const std::string& haystack, const std::string& needle);

/// Run each readiness condition in `waits` in order, under a single shared
/// deadline (`timeout` from the moment this is called). Throws DockerError if
/// the deadline passes before a condition is met. `tty` records whether the
/// container was created with Tty=true, so log-based waits read its raw/unframed
/// stream instead of demuxing (which would garble it).
///
/// Polling-based for now (see TODO for the follow-stream optimization).
void wait_until_ready(DockerClient& client, const std::string& id,
                      const std::vector<WaitFor>& waits, std::chrono::milliseconds timeout,
                      bool tty = false);

} // namespace detail
} // namespace testcontainers
