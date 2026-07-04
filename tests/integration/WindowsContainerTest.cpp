#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon in WINDOWS-containers mode):
//   WindowsContainer.EchoExitsWithExpectedLogs - a nanoserver container runs `cmd /c echo` to completion and its stdout carries the printed marker.
//   WindowsContainer.ExecRunsInRunningContainer - exec'ing `cmd /c echo` inside a running nanoserver container captures stdout and exit code 0.

using namespace testcontainers;

namespace {

// Windows base image: nanoserver ships cmd.exe and ping.exe, which is all
// these tests need. The tag is resolved from the DAEMON's Windows build below.
constexpr const char* kWindowsImage = "mcr.microsoft.com/windows/nanoserver";

/// The nanoserver tag matching the daemon host's Windows build. Process
/// isolation (the only mode on CI runners — no nested virtualization, so no
/// Hyper-V isolation) requires the container build to MATCH the host build,
/// so the tag cannot be hardcoded. Returns "" for a build with no known tag.
std::string nanoserver_tag_for(DockerClient& client) {
    // /version on a Windows daemon: "KernelVersion":"10.0 26100 (26100. ..."
    const auto res = client.request("GET", "/version");
    const std::string marker = "\"KernelVersion\":\"10.0 ";
    const std::size_t at = res.body.find(marker);
    if (at == std::string::npos) {
        return "";
    }
    const long build = std::strtol(res.body.c_str() + at + marker.size(), nullptr, 10);
    switch (build) {
    case 17763: return "ltsc2019";
    case 20348: return "ltsc2022";
    case 26100: return "ltsc2025";
    default:    return "";
    }
}

} // namespace

// Skipped unless the daemon is in Windows-containers mode (the only mode that
// can run these images); on an UNKNOWN Windows build the tests FAIL rather
// than skip, so a new Windows release cannot silently turn this suite into a
// false positive. The first run pulls a large Windows base layer — expect it
// (GitHub windows-2022 runners have nanoserver:ltsc2022 pre-cached).
class WindowsContainer : public ::testing::Test {
protected:
    void SetUp() override {
        if (auto why = tcit::windows_engine_unavailable()) {
            GTEST_SKIP() << *why;
        }
        DockerClient client = DockerClient::from_environment();
        tag_ = nanoserver_tag_for(client);
        ASSERT_FALSE(tag_.empty())
            << "no nanoserver tag known for this daemon's Windows build - "
               "extend nanoserver_tag_for() (GET /version KernelVersion: "
            << client.request("GET", "/version").body.substr(0, 512) << ")";
    }

    std::string tag_;
};

TEST_F(WindowsContainer, EchoExitsWithExpectedLogs) {
    // The robust exit + logs pattern: run a short command to completion, then
    // read its captured stdout. No long-running keep-alive needed.
    Container c = GenericImage(kWindowsImage, tag_)
                      .with_cmd({"cmd", "/c", "echo hello-windows"})
                      .with_wait(wait_for::exit())
                      .start();

    EXPECT_NE(c.logs().stdout_data.find("hello-windows"), std::string::npos);
}

TEST_F(WindowsContainer, ExecRunsInRunningContainer) {
    // Keep the container alive with a long ping so we can exec into it. nanoserver
    // ships ping.exe; `-n 60` keeps cmd.exe blocked for ~60s — plenty of time.
    Container c = GenericImage(kWindowsImage, tag_)
                      .with_cmd({"cmd", "/c", "ping -n 60 127.0.0.1 >nul"})
                      .start();

    ASSERT_TRUE(c.is_running());

    const ExecResult res = c.exec({"cmd", "/c", "echo hello-exec-windows"});
    EXPECT_EQ(res.exit_code, 0);
    EXPECT_NE(res.stdout_data.find("hello-exec-windows"), std::string::npos);
}
