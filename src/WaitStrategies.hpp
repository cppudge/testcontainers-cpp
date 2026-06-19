#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "testcontainers/WaitFor.hpp"

namespace testcontainers {

class DockerClient;

namespace detail {

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
