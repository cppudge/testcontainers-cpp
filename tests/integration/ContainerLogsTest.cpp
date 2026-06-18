#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <string>
#include <thread>

#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon):
//   DockerLogs.FetchesStdoutAndStderr - a container's stdout and stderr are fetched and demultiplexed into separate streams without cross-contamination.

using namespace testcontainers;

namespace {

constexpr const char* kImage = "alpine:3.20";

// Best-effort force-remove on scope exit so tests never leak containers.
struct RemoveGuard {
    DockerClient& client;
    std::string id;
    ~RemoveGuard() {
        try {
            if (!id.empty()) {
                client.remove_container(id, /*force*/ true, /*remove_volumes*/ true);
            }
        } catch (...) {
        }
    }
};

} // namespace

// Requires a reachable Docker daemon; each test is skipped if none is available.
class DockerLogs : public ::testing::Test {
protected:
    DockerClient client = DockerClient::from_environment();

    void SetUp() override {
        if (auto why = tcit::linux_engine_unavailable()) {
            GTEST_SKIP() << *why;
        }
    }

    // Poll inspect until the container is no longer running (or we give up).
    void wait_until_exited(const std::string& id) {
        for (int i = 0; i < 100; ++i) {
            if (!client.inspect_container(id).running) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
};

TEST_F(DockerLogs, FetchesStdoutAndStderr) {
    client.pull_image(kImage);

    CreateContainerSpec spec;
    spec.image = kImage;
    spec.cmd = {"sh", "-c", "echo hello-stdout; echo hello-stderr 1>&2"};

    const std::string id = client.create_container(spec);
    ASSERT_FALSE(id.empty());
    RemoveGuard guard{client, id};

    client.start_container(id);
    wait_until_exited(id);

    const ContainerLogs logs = client.logs(id);
    EXPECT_NE(logs.stdout_data.find("hello-stdout"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_NE(logs.stderr_data.find("hello-stderr"), std::string::npos)
        << "stderr was: " << logs.stderr_data;
    // Streams must stay separated.
    EXPECT_EQ(logs.stdout_data.find("hello-stderr"), std::string::npos);
    EXPECT_EQ(logs.stderr_data.find("hello-stdout"), std::string::npos);
}
