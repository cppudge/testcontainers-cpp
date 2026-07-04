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

// Tests in this file (integration; require a Docker daemon):
//   WaitStrategies.ExitCodeWaitSucceeds - a container that runs `exit 7` with wait_for::exit_code(7) starts, becomes ready, and is no longer running.
//   WaitStrategies.HealthcheckWaitBecomesHealthy - an alpine container with a passing healthcheck and wait_for::healthy() starts and is running once healthy.
//   WaitStrategies.HttpWaitReachesNginx - an nginx container with wait_for::http("/", tcp(80), 200) starts, publishes a reachable host port, and is running.
//   WaitStrategies.PortWaitReachesRedis - a redis container with wait_for::listening_port(tcp(6379)) starts, publishes a reachable host port, and is running.
//   WaitStrategies.TimeoutThrowsStartupTimeoutError - a log wait on a message that never appears throws StartupTimeoutError (carrying the container id) and is NOT catchable as DockerError - readiness is not a daemon failure.

using namespace testcontainers;
using namespace std::chrono_literals;

// Requires a reachable Docker daemon; skipped if none is available.
class WaitStrategies : public ::testing::Test {
protected:
    void SetUp() override {
        if (auto why = tcit::linux_engine_unavailable()) {
            GTEST_SKIP() << *why;
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
    Container c =
        GenericImage("alpine", "3.20")
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
