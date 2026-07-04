#include <gtest/gtest.h>

#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/docker/Logs.hpp"

#include "WindowsEngine.hpp"

// Tests in this file (integration; require a Docker daemon in WINDOWS-containers mode):
//   WindowsContainer.EchoExitsWithExpectedLogs - a nanoserver container runs `cmd /c echo` to completion and its stdout carries the printed marker.
//   WindowsContainer.ExecRunsInRunningContainer - exec'ing `cmd /c echo` inside a running nanoserver container captures stdout and exit code 0.
//   WindowsContainer.WorkingDirApplied - with_working_dir sets the container process's cwd (`cd` prints C:\Windows).
//   WindowsContainer.TtyLogsAreRaw - a with_tty container's logs arrive raw (marker lands in stdout_data, stderr_data stays empty).

using namespace testcontainers;

// The engine guard, base-image tag resolution, and the skip-vs-fail policy for
// unknown Windows builds all live in tcit::WindowsEngineTest (WindowsEngine.hpp).
class WindowsContainer : public tcit::WindowsEngineTest {};

TEST_F(WindowsContainer, EchoExitsWithExpectedLogs) {
    // The robust exit + logs pattern: run a short command to completion, then
    // read its captured stdout. No long-running keep-alive needed.
    Container c = nanoserver()
                      .with_cmd({"cmd", "/c", "echo hello-windows"})
                      .with_wait(wait_for::exit())
                      .start();

    EXPECT_NE(c.logs().stdout_data.find("hello-windows"), std::string::npos);
}

TEST_F(WindowsContainer, ExecRunsInRunningContainer) {
    // Keep the container alive with a long ping so we can exec into it.
    Container c = nanoserver().with_cmd(keep_alive_cmd()).start();

    ASSERT_TRUE(c.is_running());

    const ExecResult res = c.exec({"cmd", "/c", "echo hello-exec-windows"});
    EXPECT_EQ(res.exit_code, 0);
    EXPECT_NE(res.stdout_data.find("hello-exec-windows"), std::string::npos);
}

TEST_F(WindowsContainer, WorkingDirApplied) {
    // A bare `cd` prints the process's working directory.
    Container c = nanoserver()
                      .with_working_dir("C:\\Windows")
                      .with_cmd({"cmd", "/c", "cd"})
                      .with_wait(wait_for::exit())
                      .start();

    const std::string out = c.logs().stdout_data;
    EXPECT_NE(out.find("C:\\Windows"), std::string::npos) << "stdout was: " << out;
}

TEST_F(WindowsContainer, TtyLogsAreRaw) {
    // A TTY stream is raw/unframed: logs() reads it without demuxing, the
    // marker lands in stdout_data and there is no separate stderr channel.
    Container c = nanoserver()
                      .with_tty()
                      .with_cmd({"cmd", "/c", "echo tty-hello-windows"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("tty-hello-windows"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_TRUE(logs.stderr_data.empty());
}
