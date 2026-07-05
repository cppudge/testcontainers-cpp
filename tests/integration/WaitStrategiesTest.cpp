#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/Healthcheck.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"
#include "WindowsEngine.hpp"

// Tests in this file (integration; require a Docker daemon — Linux mode for the
// WaitStrategies suite, Windows mode for the WindowsWaitStrategies mirror):
//   WaitStrategies.ExitCodeWaitSucceeds - a container that runs `exit 7` with wait_for::exit_code(7) starts, becomes ready, and is no longer running.
//   WaitStrategies.HealthcheckWaitBecomesHealthy - an alpine container with a passing healthcheck and wait_for::healthy() starts and is running once healthy.
//   WaitStrategies.HttpWaitReachesNginx - an nginx container with wait_for::http("/", tcp(80), 200) starts, publishes a reachable host port, and is running.
//   WaitStrategies.PortWaitReachesRedis - a redis container with wait_for::listening_port(tcp(6379)) starts, publishes a reachable host port, and is running.
//   WaitStrategies.TimeoutThrowsStartupTimeoutError - a log wait on a message that never appears throws StartupTimeoutError (carrying the container id) and is NOT catchable as DockerError - readiness is not a daemon failure.
//   WaitStrategies.LogMessageAppearsLate - a marker echoed ~1s after start is caught by the streaming log wait while the container keeps running.
//   WaitStrategies.LogWaitSucceedsOnExitedContainer - after an exit wait guarantees the container is gone, the log wait still matches the marker in the exited container's log HISTORY (the follow stream ends right after replaying it).
//   WaitStrategies.LogWaitCountsRepeatedMessage - times=2 gates on the SECOND occurrence: the wait returns only after the delayed repeat is streamed.
//   WindowsWaitStrategies.ExitCodeWaitSucceeds - cmd `exit 7` with wait_for::exit_code(7) on a Windows container.
//   WindowsWaitStrategies.StdoutMessageWait - wait_for::stdout_message gates on a marker echoed by a Windows container that keeps running.
//   WindowsWaitStrategies.HealthcheckWaitBecomesHealthy - a Windows container with a passing shell healthcheck reaches healthy.
//   WindowsWaitStrategies.ListeningPortWaitOnServercore - a PowerShell TcpListener in servercore + wait_for::listening_port proves a published Windows port is reachable from the host.

using namespace testcontainers;
using namespace std::chrono_literals;

// Requires a reachable Docker daemon; skipped if none is available.
class WaitStrategies : public ::testing::Test {
protected:
    void SetUp() override {
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

TEST_F(WaitStrategies, ExitCodeWaitSucceeds) {
    Container c = GenericImage("alpine", "3.20")
                      .with_cmd({"sh", "-c", "exit 7"})
                      .with_wait(wait_for::exit_code(7))
                      .start();
    // The wait succeeded only because the container exited with code 7.
    EXPECT_FALSE(c.is_running());
}

TEST_F(WaitStrategies, HealthcheckWaitBecomesHealthy) {
    Container c = GenericImage("alpine", "3.20")
                      .with_cmd({"sleep", "60"})
                      .with_healthcheck(Healthcheck::cmd_shell("exit 0")
                                            .with_interval(500ms)
                                            .with_retries(3)
                                            .with_start_period(0ms))
                      .with_wait(wait_for::healthy())
                      .start();
    EXPECT_TRUE(c.is_running());
}

TEST_F(WaitStrategies, HttpWaitReachesNginx) {
    Container c = GenericImage("nginx", "1.27-alpine")
                      .with_exposed_port(tcp(80))
                      .with_wait(wait_for::http("/", tcp(80), 200))
                      .start();
    EXPECT_GT(c.get_host_port(tcp(80)), 0);
    EXPECT_TRUE(c.is_running());
}

TEST_F(WaitStrategies, PortWaitReachesRedis) {
    Container c = GenericImage("redis", "7.2")
                      .with_exposed_port(tcp(6379))
                      .with_wait(wait_for::listening_port(tcp(6379)))
                      .start();
    EXPECT_GT(c.get_host_port(tcp(6379)), 0);
    EXPECT_TRUE(c.is_running());
}

TEST_F(WaitStrategies, TimeoutThrowsStartupTimeoutError) {
    // A log line that never appears: the wait can only end by timing out. (A
    // port wait would NOT work here — Docker Desktop's host proxy accepts on a
    // published port even when nothing listens inside the container.)
    try {
        GenericImage("alpine", "3.20")
            .with_cmd({"sleep", "60"})
            .with_wait(wait_for::log("NEVER_LOGGED_9c41"))
            .with_startup_timeout(3s)
            .start();
        FAIL() << "expected StartupTimeoutError";
    } catch (const DockerError& e) {
        // The headline contract of the error model: a readiness timeout is not
        // a daemon failure, so it must NOT be catchable as DockerError.
        FAIL() << "readiness timeout arrived as DockerError: " << e.what();
    } catch (const StartupTimeoutError& e) {
        EXPECT_FALSE(e.resource_id().empty()) << e.what(); // the container id
        EXPECT_NE(std::string(e.what()).find("Timed out waiting for log message"),
                  std::string::npos)
            << e.what();
    }
}

TEST_F(WaitStrategies, LogMessageAppearsLate) {
    // The marker is NOT in the initial history — it arrives ~1s into the
    // follow stream, so this exercises the live half of the streaming wait.
    Container c = GenericImage("alpine", "3.20")
                      .with_cmd({"sh", "-c", "sleep 1; echo tc-late-marker; sleep 60"})
                      .with_wait(wait_for::log("tc-late-marker"))
                      .start();
    EXPECT_TRUE(c.is_running());
}

TEST_F(WaitStrategies, LogWaitSucceedsOnExitedContainer) {
    // Waits run in order: the exit wait GUARANTEES the container is gone
    // before the log wait even starts, so the log wait can only succeed by
    // replaying the exited container's log history (the follow stream ends
    // right after delivering it). A single log wait would be racy the other
    // way around — streaming catches the marker at echo time, often while the
    // container is still alive.
    Container c = GenericImage("alpine", "3.20")
                      .with_cmd({"sh", "-c", "echo tc-done-marker"})
                      .with_wait(wait_for::exit())
                      .with_wait(wait_for::log("tc-done-marker"))
                      .start();
    EXPECT_FALSE(c.is_running());
}

TEST_F(WaitStrategies, LogWaitCountsRepeatedMessage) {
    // Two occurrences, the second delayed: the wait must count across the
    // history/live boundary and return only once both have streamed.
    Container c =
        GenericImage("alpine", "3.20")
            .with_cmd({"sh", "-c", "echo tc-twice-marker; sleep 1; echo tc-twice-marker; sleep 60"})
            .with_wait(wait_for::log("tc-twice-marker", 2))
            .start();
    EXPECT_TRUE(c.is_running());
}

// The Windows mirror. The http wait has no Windows twin (it would need a real
// HTTP server in the container — nginx-scale, not worth a multi-GB image); the
// listening-port wait uses a PowerShell TcpListener in servercore instead.
class WindowsWaitStrategies : public tcit::WindowsEngineTest {};

TEST_F(WindowsWaitStrategies, ExitCodeWaitSucceeds) {
    Container c =
        nanoserver().with_cmd({"cmd", "/c", "exit 7"}).with_wait(wait_for::exit_code(7)).start();
    // The wait succeeded only because the container exited with code 7.
    EXPECT_FALSE(c.is_running());
}

TEST_F(WindowsWaitStrategies, StdoutMessageWait) {
    // Echo the marker, then keep running: the wait must return on the log
    // line, not on container exit.
    Container c = nanoserver()
                      .with_cmd({"cmd", "/c", "echo tc-ready-win & ping -n 300 127.0.0.1 >nul"})
                      .with_wait(wait_for::stdout_message("tc-ready-win"))
                      .start();
    EXPECT_TRUE(c.is_running());
}

TEST_F(WindowsWaitStrategies, HealthcheckWaitBecomesHealthy) {
    // The shell form runs under cmd /S /C on a Windows daemon; `exit 0` is the
    // trivially healthy probe.
    Container c = nanoserver()
                      .with_cmd(keep_alive_cmd())
                      .with_healthcheck(Healthcheck::cmd_shell("exit 0")
                                            .with_interval(500ms)
                                            .with_retries(3)
                                            .with_start_period(0ms))
                      .with_wait(wait_for::healthy())
                      .start();
    EXPECT_TRUE(c.is_running());
}

TEST_F(WindowsWaitStrategies, ListeningPortWaitOnServercore) {
    // A real in-container listener (nanoserver ships none, servercore has
    // PowerShell): the listening_port wait then proves the published Windows
    // port actually accepts a host connection — the strongest port assertion
    // available on this engine.
    // Two waits: the stdout marker separates "PowerShell never came up" from
    // "port not reachable" when the port wait times out. Two minutes of budget:
    // PowerShell's cold start (.NET JIT in a fresh container) can eat tens of
    // seconds on a loaded 2-core CI runner — the default 60s margin is thin.
    Container c =
        servercore()
            .with_cmd({"powershell", "-NoLogo", "-NoProfile", "-Command",
                       // One PowerShell one-liner, split across adjacent literals.
                       // NOLINTNEXTLINE(bugprone-suspicious-missing-comma)
                       "$l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Any, "
                       "8080); $l.Start(); Write-Host tc-listening; "
                       "while ($true) { Start-Sleep -Seconds 1 }"})
            .with_exposed_port(tcp(8080))
            .with_wait(wait_for::stdout_message("tc-listening"))
            .with_wait(wait_for::listening_port(tcp(8080)))
            .with_startup_timeout(std::chrono::minutes(2))
            .start();

    EXPECT_GT(c.get_host_port(tcp(8080)), 0);
    EXPECT_TRUE(c.is_running());
}
