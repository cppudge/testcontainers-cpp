#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "Reaper.hpp"
#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/DockerComposeContainer.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"
#include "RedisPing.hpp"

// Tests in this file (integration; require a Linux Docker daemon):
//   Compose.LocalClientBringsUpRedis - the LOCAL client (host `docker compose` CLI; default) brings up redis from a temp YAML, the published host port answers a raw TCP PING with PONG, and stop() removes every container carrying the project label. Skipped if the host has no `docker compose`.
//   Compose.ContainerisedClientBringsUpRedis - the CONTAINERISED client (long-lived docker:26.1-cli + exec) brings up redis (pulling docker:26.1-cli on first run), PING/PONG succeeds, and teardown leaves nothing.
//   Compose.AutoClientBringsUpRedis - the AUTO client (local first, else containerised) brings up redis, PING/PONG succeeds, and teardown leaves nothing.
//   Compose.RestartKeepsProjectAlive - start() on an already-started handle tears the OLD run down first, so the fresh containers survive (the shared project label makes teardown-after-up remove the new run's containers).
//   Compose.ProjectFilterRegisteredWithReaper - start() hands the session's Ryuk an extra `label=com.docker.compose.project=<project>` filter (crash-safe reaping of the stack), exactly once across a restart.

using namespace testcontainers;

namespace {

using tcit::redis_ping;

// A compose file publishing redis 6379 to a random host port.
constexpr const char* kRedisYaml = R"(services:
  redis:
    image: redis:7.2
    ports:
      - "6379"
)";

// True if the host `docker compose` CLI is available (for the local-client guard).
bool host_docker_compose_available() {
    // popen/_pclose: `docker compose version` exiting 0 means the plugin is there.
#if defined(_WIN32)
    FILE* pipe = _popen("docker compose version 2>&1", "r");
#else
    FILE* pipe = popen("docker compose version 2>&1", "r");
#endif
    if (pipe == nullptr) {
        return false;
    }
    std::array<char, 256> buf{};
    while (std::fread(buf.data(), 1, buf.size(), pipe) > 0) {
        // drain
    }
#if defined(_WIN32)
    const int status = _pclose(pipe);
    return status == 0;
#else
    const int status = pclose(pipe);
    return status == 0;
#endif
}

// Bring `compose` up, assert redis answers PING with PONG on its published port,
// tear down, and assert no container with the project label remains.
void run_redis_roundtrip(DockerComposeContainer& compose) {
    const std::string project_name = compose.project_name();

    ASSERT_NO_THROW(compose.start());

    const std::uint16_t host_port = compose.get_service_port("redis", tcp(6379));
    EXPECT_GT(host_port, 0);
    const std::string host = compose.get_service_host("redis");

    std::string reply;
    ASSERT_NO_THROW(reply = redis_ping(host, host_port));
    EXPECT_NE(reply.find("PONG"), std::string::npos);

    ASSERT_NO_THROW(compose.stop());

    DockerClient client = DockerClient::from_environment();
    const auto leftovers = client.list_containers(
        {{"label", "com.docker.compose.project=" + project_name}}, /*all*/ true);
    EXPECT_TRUE(leftovers.empty());
}

} // namespace

// Requires a reachable Linux Docker daemon; skipped otherwise.
class Compose : public ::testing::Test {
protected:
    void SetUp() override {
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

TEST_F(Compose, LocalClientBringsUpRedis) {
    if (!host_docker_compose_available()) {
        GTEST_SKIP(); // host `docker compose` CLI is not available
    }
    // from_yaml writes a temp file so the local client has a real file on disk.
    DockerComposeContainer compose =
        DockerComposeContainer::from_yaml(kRedisYaml).with_exposed_service("redis", tcp(6379));
    run_redis_roundtrip(compose);
}

TEST_F(Compose, ContainerisedClientBringsUpRedis) {
    // The containerised client pulls docker:26.1-cli on the first run — allow time.
    DockerComposeContainer compose = DockerComposeContainer::from_yaml(kRedisYaml)
                                         .with_client(ComposeClientKind::Containerised)
                                         .with_exposed_service("redis", tcp(6379));
    run_redis_roundtrip(compose);
}

TEST_F(Compose, RestartKeepsProjectAlive) {
    if (!host_docker_compose_available()) {
        GTEST_SKIP(); // host `docker compose` CLI is not available
    }
    DockerComposeContainer compose =
        DockerComposeContainer::from_yaml(kRedisYaml).with_exposed_service("redis", tcp(6379));
    ASSERT_NO_THROW(compose.start());

    // Both runs share the project label, so tearing the old run down AFTER the
    // new `up` would remove the fresh containers; a correct restart tears down
    // first. The service must be alive and reachable after the second start().
    ASSERT_NO_THROW(compose.start());
    const std::string id = compose.get_service_container_id("redis");
    DockerClient client = DockerClient::from_environment();
    EXPECT_TRUE(client.inspect_container(id).running) << "restart removed its own container";
    std::string reply;
    ASSERT_NO_THROW(reply = redis_ping(compose.get_service_host("redis"),
                                       compose.get_service_port("redis", tcp(6379))));
    EXPECT_NE(reply.find("PONG"), std::string::npos);

    ASSERT_NO_THROW(compose.stop());
    const auto leftovers = client.list_containers(
        {{"label", "com.docker.compose.project=" + compose.project_name()}}, /*all*/ true);
    EXPECT_TRUE(leftovers.empty());
}

TEST_F(Compose, ProjectFilterRegisteredWithReaper) {
    if (detail::ryuk_disabled()) {
        GTEST_SKIP(); // no reaper to register with
    }
    if (!host_docker_compose_available()) {
        GTEST_SKIP(); // host `docker compose` CLI is not available
    }
    DockerComposeContainer compose =
        DockerComposeContainer::from_yaml(kRedisYaml).with_exposed_service("redis", tcp(6379));
    ASSERT_NO_THROW(compose.start());

    // The filter line must have been ACKed by the REAL Ryuk (register_filter
    // throws into start() otherwise), and recorded once.
    const std::string expected =
        detail::ryuk_filter_line("com.docker.compose.project", compose.project_name());
    const auto count_registered = [&expected] {
        const std::vector<std::string> filters = detail::Reaper::instance().registered_filters();
        return std::count(filters.begin(), filters.end(), expected);
    };
    EXPECT_EQ(count_registered(), 1);

    // A restart shares the project name — the filter must not be re-sent.
    ASSERT_NO_THROW(compose.start());
    EXPECT_EQ(count_registered(), 1);

    ASSERT_NO_THROW(compose.stop());
}

TEST_F(Compose, AutoClientBringsUpRedis) {
    // Auto picks Local when the host has `docker compose` (the case here), else
    // falls back to Containerised. Either way redis must come up. from_yaml writes
    // a temp file so the local path has a real file on disk; with_client(Auto) is
    // the with_auto_client form applied to that temp file.
    DockerComposeContainer compose = DockerComposeContainer::from_yaml(kRedisYaml)
                                         .with_client(ComposeClientKind::Auto)
                                         .with_exposed_service("redis", tcp(6379));
    run_redis_roundtrip(compose);
}
