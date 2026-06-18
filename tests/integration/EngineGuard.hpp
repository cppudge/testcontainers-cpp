#pragma once

#include <exception>
#include <optional>
#include <string>

#include "testcontainers/docker/DockerClient.hpp"

// Engine-aware skip helpers for integration tests. These return a skip *reason*
// (or std::nullopt when the test may run) instead of calling GTEST_SKIP
// themselves, so they are safe to call from a fixture's SetUp():
//
//   void SetUp() override {
//       if (auto why = tcit::linux_engine_unavailable()) GTEST_SKIP() << *why;
//   }
//
// Docker Desktop can be switched between Linux- and Windows-containers modes;
// the daemon only runs images matching the current mode. Linux integration
// tests skip when the engine is Windows (and vice versa) so the suite stays
// green whichever mode the host is in.
namespace tcit {

/// A reason the LINUX engine is unavailable, or nullopt if it is usable:
/// the daemon is unreachable, OR the engine is in Windows-containers mode.
inline std::optional<std::string> linux_engine_unavailable() {
    try {
        testcontainers::DockerClient client = testcontainers::DockerClient::from_environment();
        if (!client.ping()) {
            return std::string("Docker daemon did not respond to /_ping");
        }
        if (client.is_windows_engine()) {
            return std::string("Docker engine is in Windows-containers mode; "
                               "this Linux-image test cannot run");
        }
        return std::nullopt;
    } catch (const std::exception& e) {
        return std::string("Docker not available: ") + e.what();
    }
}

/// A reason the WINDOWS engine is unavailable, or nullopt if it is usable:
/// the daemon is unreachable, OR the engine is in Linux-containers mode.
inline std::optional<std::string> windows_engine_unavailable() {
    try {
        testcontainers::DockerClient client = testcontainers::DockerClient::from_environment();
        if (!client.ping()) {
            return std::string("Docker daemon did not respond to /_ping");
        }
        if (!client.is_windows_engine()) {
            return std::string("Docker engine is in Linux-containers mode; "
                               "this Windows-container test cannot run");
        }
        return std::nullopt;
    } catch (const std::exception& e) {
        return std::string("Docker not available: ") + e.what();
    }
}

} // namespace tcit
