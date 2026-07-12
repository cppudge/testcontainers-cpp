#pragma once

#include <gtest/gtest.h>

#include <exception>
#include <optional>
#include <string>

#include "testcontainers/docker/DockerClient.hpp"

// Engine-aware skip helpers for integration tests. These return a skip *reason*
// (or std::nullopt when the test may run) instead of calling GTEST_SKIP
// themselves, so they are safe to call from a fixture's SetUp() —
// LinuxEngineTest below is that fixture, pre-built.
//
// Docker Desktop can be switched between Linux- and Windows-containers modes;
// the daemon only runs images matching the current mode. Linux integration
// tests skip when the engine is Windows (and vice versa) so the suite stays
// green whichever mode the host is in.
//
// The reason string is deliberately NOT streamed into GTEST_SKIP at the call
// sites: every single-engine CI run mass-skips the other engine's suites, and
// dozens of identical reason lines drown the output that matters. The helpers
// still RETURN the reason so a debugging session can print it on demand.
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

/// The standard fixture for Linux-engine suites: SetUp() skips the test when
/// linux_engine_unavailable(). Derive (adding members freely); an override of
/// SetUp() must call this one first. The Windows twin — with its build-matched
/// image plumbing — is tcit::WindowsEngineTest in WindowsEngine.hpp.
class LinuxEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

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
