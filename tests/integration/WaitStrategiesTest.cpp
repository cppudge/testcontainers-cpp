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
