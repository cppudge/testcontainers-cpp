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
//   ContainerConfig.UlimitApplied - a ulimit is applied so the soft nofile limit reported inside the container matches.
//   ContainerConfig.ExtraHostApplied - an extra host (via the typed setter) resolves to its mapped IP inside the container.
//   ContainerConfig.CustomSubstitutorRewritesImage - a custom image-name substitutor rewrites a bogus reference to a runnable one used at create.
//   ContainerConfig.AlwaysPullPolicyStarts - ImagePullPolicy::Always pulls before create and the container still starts and runs.

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

TEST_F(ContainerConfig, UlimitApplied) {
    Container c = GenericImage("alpine", "3.20")
                      .with_cmd({"sh", "-c", "ulimit -n"})
                      .with_ulimit("nofile", 4096, 8192)
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("4096"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, ExtraHostApplied) {
    Container c = GenericImage("alpine", "3.20")
                      .with_extra_host("myhost", "1.2.3.4")
                      .with_cmd({"sh", "-c", "getent hosts myhost || cat /etc/hosts"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("1.2.3.4"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_NE(logs.stdout_data.find("myhost"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, CustomSubstitutorRewritesImage) {
    // The original reference is bogus; the substitutor must be what reaches create
    // (otherwise the bogus image would fail to pull). Proves the rewrite flows in.
    Container c = GenericImage("nonexistent-image-name", "v0")
                      .with_image_name_substitutor(
                          [](const std::string&) { return std::string("alpine:3.20"); })
                      .with_cmd({"sh", "-c", "echo substituted"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("substituted"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, AlwaysPullPolicyStarts) {
    // Exercises the always-pull-before-create path; alpine:3.20 is public so the
    // pull succeeds and the container still runs.
    Container c = GenericImage("alpine", "3.20")
                      .with_image_pull_policy(ImagePullPolicy::Always)
                      .with_cmd({"sh", "-c", "echo ok"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("ok"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}
