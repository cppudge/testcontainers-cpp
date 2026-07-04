#pragma once

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "testcontainers/GenericImage.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"

// Shared plumbing for integration tests that need a daemon in WINDOWS-containers
// mode: the nanoserver base image whose tag matches the daemon host's build
// (process isolation requires an exact build match), plus a fixture that skips
// on any other engine. Derive from tcit::WindowsEngineTest and use nanoserver()
// / keep_alive_cmd() the way Linux suites use alpine + `sleep`.
namespace tcit {

// Windows base image: nanoserver ships cmd.exe and ping.exe, which is all
// most tests need. The tag is resolved from the DAEMON's Windows build below.
inline constexpr const char* kWindowsImage = "mcr.microsoft.com/windows/nanoserver";

// The bigger base with PowerShell, for tests that need an in-container TCP
// listener (nanoserver has no server binary). Same build-matched tag scheme;
// GitHub windows runners pre-cache it, locally the first use pulls ~2.5 GB.
inline constexpr const char* kWindowsServercoreImage = "mcr.microsoft.com/windows/servercore";

/// The nanoserver tag matching the daemon host's Windows build. Process
/// isolation (the only mode on CI runners — no nested virtualization, so no
/// Hyper-V isolation) requires the container build to MATCH the host build,
/// so the tag cannot be hardcoded. Returns "" for a build with no known tag.
inline std::string nanoserver_tag_for(testcontainers::DockerClient& client) {
    // /version on a Windows daemon: "KernelVersion":"10.0 26100 (26100. ..."
    const auto res = client.request("GET", "/version");
    const std::string marker = "\"KernelVersion\":\"10.0 ";
    const std::size_t at = res.body.find(marker);
    if (at == std::string::npos) {
        return "";
    }
    const long build = std::strtol(res.body.c_str() + at + marker.size(), nullptr, 10);
    switch (build) {
    case 17763:
        return "ltsc2019";
    case 20348:
        return "ltsc2022";
    case 26100:
        return "ltsc2025";
    default:
        return "";
    }
}

// Skipped unless the daemon is in Windows-containers mode (the only mode that
// can run these images); on an UNKNOWN Windows build the tests FAIL rather
// than skip, so a new Windows release cannot silently turn a suite into a
// false positive. The first run pulls a large Windows base layer — expect it
// (GitHub windows-2022 runners have nanoserver:ltsc2022 pre-cached).
class WindowsEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (tcit::windows_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / not Windows-containers mode; reason not streamed (CI noise)
        }
        testcontainers::DockerClient client = testcontainers::DockerClient::from_environment();
        tag_ = nanoserver_tag_for(client);
        ASSERT_FALSE(tag_.empty()) << "no nanoserver tag known for this daemon's Windows build - "
                                      "extend nanoserver_tag_for() (GET /version KernelVersion: "
                                   << client.request("GET", "/version").body.substr(0, 512) << ")";
    }

    /// The build-matched nanoserver base, ready for with_* chaining. Pinned to
    /// PROCESS isolation: Docker Desktop defaults Windows containers to Hyper-V
    /// isolation, under which the daemon rejects filesystem operations against
    /// a running container (copy_to / read_file — HTTP 500). Process isolation
    /// needs the image build to match the host build, which nanoserver_tag_for
    /// already guarantees; CI runners use process isolation regardless (no
    /// nested virtualization).
    testcontainers::GenericImage nanoserver() const {
        return testcontainers::GenericImage(kWindowsImage, tag_).with_isolation("process");
    }

    /// The build-matched servercore base (PowerShell inside), same isolation
    /// rationale as nanoserver().
    testcontainers::GenericImage servercore() const {
        return testcontainers::GenericImage(kWindowsServercoreImage, tag_)
            .with_isolation("process");
    }

    /// The nanoserver equivalent of `sleep 300`: ping.exe blocks cmd.exe for
    /// ~5 minutes, keeping the container alive for exec/copy against it.
    static std::vector<std::string> keep_alive_cmd() {
        return {"cmd", "/c", "ping -n 300 127.0.0.1 >nul"};
    }

    std::string tag_;
};

} // namespace tcit
