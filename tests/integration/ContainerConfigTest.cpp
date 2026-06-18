#include <gtest/gtest.h>

#include <exception>
#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/Logs.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon):
//   ContainerConfig.WorkingDirAndUser - working dir and user are applied so the process runs in /tmp as uid 1000.
//   ContainerConfig.EntrypointOverride - an explicit entrypoint overrides the image default so `echo` prints the cmd arg.
//   ContainerConfig.TmpfsMount - a tmpfs mount appears as a tmpfs filesystem at its target in /proc/mounts.

using namespace testcontainers;

// Requires a reachable Docker daemon; skipped if none is available. The
// short-lived containers below use wait_for::exit(), so start() returns only
// after the process finishes; logs() then reads its captured output. The
// Container still auto-removes on scope exit.
class ContainerConfig : public ::testing::Test {
protected:
    void SetUp() override {
        if (auto why = tcit::linux_engine_unavailable()) {
            GTEST_SKIP() << *why;
        }
    }
};

TEST_F(ContainerConfig, WorkingDirAndUser) {
    Container c = GenericImage("alpine", "3.20")
                      .with_working_dir("/tmp")
                      .with_user("1000:1000")
                      .with_cmd({"sh", "-c", "pwd; id -u"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("/tmp"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_NE(logs.stdout_data.find("1000"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, EntrypointOverride) {
    Container c = GenericImage("alpine", "3.20")
                      .with_entrypoint({"echo"})
                      .with_cmd({"hello-entry"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("hello-entry"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, TmpfsMount) {
    Container c = GenericImage("alpine", "3.20")
                      .with_mount(Mount::tmpfs("/cache"))
                      .with_cmd({"sh", "-c", "grep /cache /proc/mounts"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("/cache"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_NE(logs.stdout_data.find("tmpfs"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}
