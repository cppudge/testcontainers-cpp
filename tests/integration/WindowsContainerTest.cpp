#include <gtest/gtest.h>

#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon in WINDOWS-containers mode):
//   WindowsContainer.EchoExitsWithExpectedLogs - a nanoserver container runs `cmd /c echo` to completion and its stdout carries the printed marker.
//   WindowsContainer.ExecRunsInRunningContainer - exec'ing `cmd /c echo` inside a running nanoserver container captures stdout and exit code 0.

using namespace testcontainers;

namespace {

// Windows base image. `ltsc2025` matches this host (Windows build 26100); the
// older `ltsc2022` fails with "container operating system does not match host"
// here. nanoserver ships cmd.exe and ping.exe, which is all these tests need.
constexpr const char* kWindowsImage = "mcr.microsoft.com/windows/nanoserver";
constexpr const char* kWindowsTag = "ltsc2025";

} // namespace

// Skipped unless the daemon is in Windows-containers mode (the only mode that can
// run these images). The first run pulls a large Windows base layer — expect it.
class WindowsContainer : public ::testing::Test {
protected:
    void SetUp() override {
        if (auto why = tcit::windows_engine_unavailable()) {
            GTEST_SKIP() << *why;
        }
    }
};

TEST_F(WindowsContainer, EchoExitsWithExpectedLogs) {
    // The robust exit + logs pattern: run a short command to completion, then
    // read its captured stdout. No long-running keep-alive needed.
    Container c = GenericImage(kWindowsImage, kWindowsTag)
                      .with_cmd({"cmd", "/c", "echo hello-windows"})
                      .with_wait(wait_for::exit())
                      .start();

    EXPECT_NE(c.logs().stdout_data.find("hello-windows"), std::string::npos);
}

TEST_F(WindowsContainer, ExecRunsInRunningContainer) {
    // Keep the container alive with a long ping so we can exec into it. nanoserver
    // ships ping.exe; `-n 60` keeps cmd.exe blocked for ~60s — plenty of time.
    Container c = GenericImage(kWindowsImage, kWindowsTag)
                      .with_cmd({"cmd", "/c", "ping -n 60 127.0.0.1 >nul"})
                      .start();

    ASSERT_TRUE(c.is_running());

    const ExecResult res = c.exec({"cmd", "/c", "echo hello-exec-windows"});
    EXPECT_EQ(res.exit_code, 0);
    EXPECT_NE(res.stdout_data.find("hello-exec-windows"), std::string::npos);
}
