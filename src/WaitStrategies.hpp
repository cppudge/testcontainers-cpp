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
/// the deadline passes before a condition is met.
///
/// Polling-based for now (see TODO for the follow-stream optimization).
void wait_until_ready(DockerClient& client, const std::string& id,
                      const std::vector<WaitFor>& waits, std::chrono::milliseconds timeout);

} // namespace detail
} // namespace testcontainers
