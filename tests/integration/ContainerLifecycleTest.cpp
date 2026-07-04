#include <gtest/gtest.h>

#include <exception>
#include <string>

#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon):
//   DockerLifecycle.CreateStartInspectRemove - a container is created, started, seen running, stopped, and removed, after which inspect fails.
//   DockerLifecycle.LazyPullOnCreate - create_container transparently pulls a missing image instead of failing with 404.
//   DockerLifecycle.PublishesExposedPort - an exposed port with publish-all gets a non-zero host port reported by inspect.

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
            // Best-effort: a guard must never throw from its destructor.
        }
    }
};

} // namespace

// Requires a reachable Docker daemon; each test is skipped if none is available.
class DockerLifecycle : public ::testing::Test {
protected:
    DockerClient client = DockerClient::from_environment();

    void SetUp() override {
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

TEST_F(DockerLifecycle, CreateStartInspectRemove) {
    client.pull_image(kImage);

    CreateContainerSpec spec;
    spec.image = kImage;
    spec.cmd = {"sleep", "60"};
    spec.labels = {{"org.testcontainers.managed-by", "testcontainers-cpp"}};

    const std::string id = client.create_container(spec);
    ASSERT_FALSE(id.empty());
    RemoveGuard guard{client, id};

    client.start_container(id);

    const auto info = client.inspect_container(id);
    EXPECT_TRUE(info.running);
    EXPECT_EQ(info.status, "running");

    client.stop_container(id, 1);
    client.remove_container(id, true, true);
    guard.id.clear(); // already removed

    EXPECT_THROW(client.inspect_container(id), DockerError);
}

TEST_F(DockerLifecycle, LazyPullOnCreate) {
    // create_container should transparently pull the image when missing
    // (rather than failing with 404).
    CreateContainerSpec spec;
    spec.image = kImage;
    spec.cmd = {"true"};

    const std::string id = client.create_container(spec);
    RemoveGuard guard{client, id};
    EXPECT_FALSE(id.empty());
}

TEST_F(DockerLifecycle, PublishesExposedPort) {
    client.pull_image(kImage);

    CreateContainerSpec spec;
    spec.image = kImage;
    spec.cmd = {"sleep", "60"};
    spec.exposed_ports = {"8080/tcp"};
    spec.publish_all_ports = true;

    const std::string id = client.create_container(spec);
    RemoveGuard guard{client, id};
    client.start_container(id);

    const auto info = client.inspect_container(id);
    ASSERT_TRUE(info.running);
    ASSERT_EQ(info.ports.count("8080/tcp"), 1u);
    ASSERT_FALSE(info.ports.at("8080/tcp").empty());
    EXPECT_GT(info.ports.at("8080/tcp")[0].host_port, 0);
}
