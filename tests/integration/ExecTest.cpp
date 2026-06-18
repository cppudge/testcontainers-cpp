#include <gtest/gtest.h>

#include <exception>
#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon):
//   Exec.CapturesStdoutAndZeroExit - exec'ing `echo` in a running container captures the stdout text and reports exit code 0.
//   Exec.PropagatesNonZeroExit - exec'ing a command that exits 5 reports exit code 5.

using namespace testcontainers;

// Requires a reachable Docker daemon; skipped if none is available.
class Exec : public ::testing::Test {
protected:
    void SetUp() override {
        if (auto why = tcit::linux_engine_unavailable()) {
            GTEST_SKIP() << *why;
        }
    }
};

TEST_F(Exec, CapturesStdoutAndZeroExit) {
    // A long-running container so the exec has something to attach to; no wait
    // strategy is needed because `sleep` produces no readiness signal.
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    const ExecResult res = c.exec({"echo", "hello-exec"});
    EXPECT_NE(res.stdout_data.find("hello-exec"), std::string::npos)
        << "stdout was: " << res.stdout_data;
    EXPECT_EQ(res.exit_code, 0);
}

TEST_F(Exec, PropagatesNonZeroExit) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    const ExecResult res = c.exec({"sh", "-c", "exit 5"});
    EXPECT_EQ(res.exit_code, 5);
}
