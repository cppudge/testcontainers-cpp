#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Reaper.hpp"
#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/DockerComposeContainer.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"
#include "RedisPing.hpp"

// Tests in this file (integration; require a Linux Docker daemon):
//   Compose.LocalClientBringsUpRedis - the LOCAL client (host `docker compose` CLI; default) brings up redis from a temp YAML, the published host port answers a raw TCP PING with PONG, and stop() removes every container carrying the project label. Skipped if the host has no `docker compose`.
//   Compose.ContainerisedClientBringsUpRedis - the CONTAINERISED client (long-lived docker:26.1-cli + exec) brings up redis (pulling docker:26.1-cli on first run), PING/PONG succeeds, and teardown leaves nothing.
//   Compose.RestartKeepsProjectAlive - start() on an already-started handle tears the OLD run down first, so the fresh containers survive (the shared project label makes teardown-after-up remove the new run's containers).
//   Compose.ProfileGatesService - a service behind `profiles:` stays down without with_profile and comes up with it; the profile-aware teardown leaves no container behind.
//   Compose.ScaleRunsTwoInstances - with_scale("redis", 2) runs two discoverable instances with distinct containers and distinct ephemeral host ports (both answer PING); the plain accessor picks instance 1; an out-of-range instance throws.
//   Compose.ServiceLogsDeliverRedisStartup - follow_service_logs (deadline-bounded) sees redis's startup marker and stops via the consumer; the snapshot and its instance-1 form then both contain it.
//   Compose.AmbassadorReachesUnpublishedPort - a redis publishing NOTHING answers PING through the socat relay's published port (get_service_port resolves to it); the service itself has no host binding; teardown leaves neither containers nor the project network behind.
//   Compose.ProjectFilterRegisteredWithReaper - start() hands the session's Ryuk an extra `label=com.docker.compose.project=<project>` filter (crash-safe reaping of the stack), exactly once across a restart.
//   Compose.AutoClientBringsUpRedis - the AUTO client (local first, else containerised) brings up redis, PING/PONG succeeds, and teardown leaves nothing.

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

// A redis publishing NOTHING: only an in-network relay can reach it from the
// host (the with_ambassador case).
constexpr const char* kUnpublishedRedisYaml = R"(services:
  redis:
    image: redis:7.2
)";

// kRedisYaml plus a second service gated behind the "extra" profile (same
// image — already pulled by the other tests, and it stays alive by itself).
constexpr const char* kProfileYaml = R"(services:
  redis:
    image: redis:7.2
    ports:
      - "6379"
  extra:
    image: redis:7.2
    profiles:
      - extra
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

TEST_F(Compose, ProfileGatesService) {
    if (!host_docker_compose_available()) {
        GTEST_SKIP(); // host `docker compose` CLI is not available
    }

    // Without the profile active the gated service must not come up (the
    // profile-less redis still does — with_exposed_service probes it live).
    {
        DockerComposeContainer compose = DockerComposeContainer::from_yaml(kProfileYaml)
                                             .with_exposed_service("redis", tcp(6379));
        ASSERT_NO_THROW(compose.start());
        EXPECT_NO_THROW(compose.get_service_container_id("redis"));
        EXPECT_THROW(compose.get_service_container_id("extra"), DockerError);
        ASSERT_NO_THROW(compose.stop());
    }

    // With it active both services run, and the profile-aware `down` (backed
    // by the label sweep) leaves nothing behind.
    DockerComposeContainer compose = DockerComposeContainer::from_yaml(kProfileYaml)
                                         .with_profile("extra")
                                         .with_exposed_service("redis", tcp(6379));
    const std::string project_name = compose.project_name();
    ASSERT_NO_THROW(compose.start());

    DockerClient client = DockerClient::from_environment();
    std::string extra_id;
    ASSERT_NO_THROW(extra_id = compose.get_service_container_id("extra"));
    EXPECT_TRUE(client.inspect_container(extra_id).running);

    ASSERT_NO_THROW(compose.stop());
    const auto leftovers = client.list_containers(
        {{"label", "com.docker.compose.project=" + project_name}}, /*all*/ true);
    EXPECT_TRUE(leftovers.empty());
}

TEST_F(Compose, ScaleRunsTwoInstances) {
    if (!host_docker_compose_available()) {
        GTEST_SKIP(); // host `docker compose` CLI is not available
    }
    // kRedisYaml publishes the container port alone ("6379"), so each instance
    // gets its own ephemeral host port — the only publishing mode --scale can
    // multiply. Probe both instances explicitly.
    DockerComposeContainer compose = DockerComposeContainer::from_yaml(kRedisYaml)
                                         .with_scale("redis", 2)
                                         .with_exposed_service("redis", 1, tcp(6379))
                                         .with_exposed_service("redis", 2, tcp(6379));
    const std::string project_name = compose.project_name();
    ASSERT_NO_THROW(compose.start());

    EXPECT_EQ(compose.service_instances("redis"), (std::vector<int>{1, 2}));
    const std::string id1 = compose.get_service_container_id("redis", 1);
    const std::string id2 = compose.get_service_container_id("redis", 2);
    EXPECT_NE(id1, id2);
    EXPECT_EQ(compose.get_service_container_id("redis"), id1); // plain = instance 1

    // Both instances are live on their own ports (bound simultaneously, so the
    // ephemeral host ports must differ).
    const std::uint16_t port1 = compose.get_service_port("redis", 1, tcp(6379));
    const std::uint16_t port2 = compose.get_service_port("redis", 2, tcp(6379));
    EXPECT_NE(port1, port2);
    const std::string host = compose.get_service_host("redis");
    std::string reply;
    ASSERT_NO_THROW(reply = redis_ping(host, port1));
    EXPECT_NE(reply.find("PONG"), std::string::npos);
    ASSERT_NO_THROW(reply = redis_ping(host, port2));
    EXPECT_NE(reply.find("PONG"), std::string::npos);

    EXPECT_THROW(compose.get_service_container_id("redis", 3), DockerError);

    ASSERT_NO_THROW(compose.stop());
    DockerClient client = DockerClient::from_environment();
    const auto leftovers = client.list_containers(
        {{"label", "com.docker.compose.project=" + project_name}}, /*all*/ true);
    EXPECT_TRUE(leftovers.empty());
}

TEST_F(Compose, ServiceLogsDeliverRedisStartup) {
    if (!host_docker_compose_available()) {
        GTEST_SKIP(); // host `docker compose` CLI is not available
    }
    DockerComposeContainer compose =
        DockerComposeContainer::from_yaml(kRedisYaml).with_exposed_service("redis", tcp(6379));
    ASSERT_NO_THROW(compose.start());

    // The deadline-bounded follow replays the existing log (tail=all) and then
    // streams, so it sees redis's startup marker no matter how start() raced
    // it; the consumer stops the stream at that point.
    constexpr const char* kReadyMarker = "Ready to accept connections";
    std::string streamed;
    const FollowEnd end = compose.follow_service_logs(
        "redis",
        [&](LogSource source, std::string_view data) {
            // stdout only — the same stream the snapshot asserts below pin.
            if (source == LogSource::Stdout) {
                streamed.append(data);
            }
            return streamed.find(kReadyMarker) == std::string::npos;
        },
        std::chrono::steady_clock::now() + std::chrono::seconds(30));
    EXPECT_EQ(end, FollowEnd::ConsumerStopped);
    EXPECT_NE(streamed.find(kReadyMarker), std::string::npos);

    // The marker has certainly been written by now: the snapshot — and its
    // explicit instance-1 form — must both carry it.
    EXPECT_NE(compose.get_service_logs("redis").stdout_data.find(kReadyMarker), std::string::npos);
    EXPECT_NE(compose.get_service_logs("redis", 1).stdout_data.find(kReadyMarker),
              std::string::npos);

    ASSERT_NO_THROW(compose.stop());
}

TEST_F(Compose, AmbassadorReachesUnpublishedPort) {
    if (!host_docker_compose_available()) {
        GTEST_SKIP(); // host `docker compose` CLI is not available
    }
    DockerComposeContainer compose = DockerComposeContainer::from_yaml(kUnpublishedRedisYaml)
                                         .with_ambassador("redis", tcp(6379));
    const std::string project_name = compose.project_name();
    ASSERT_NO_THROW(compose.start());

    // The service itself publishes nothing...
    DockerClient client = DockerClient::from_environment();
    const ContainerInspect redis_info =
        client.inspect_container(compose.get_service_container_id("redis"));
    const auto bound = redis_info.ports.find("6379/tcp");
    EXPECT_TRUE(bound == redis_info.ports.end() || bound->second.empty());

    // ...yet get_service_port yields a live host door: the relay's published
    // port, PONGing straight through the compose network. Brief retry only
    // for the moment between the relay container starting and socat binding.
    const std::uint16_t host_port = compose.get_service_port("redis", tcp(6379));
    EXPECT_GT(host_port, 0);
    const std::string host = compose.get_service_host("redis");
    std::string reply;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        try {
            reply = redis_ping(host, host_port);
        } catch (const std::exception&) {
            reply.clear(); // relay not accepting yet
        }
        if (reply.find("PONG") != std::string::npos ||
            std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    EXPECT_NE(reply.find("PONG"), std::string::npos);

    // Teardown removes the relay BEFORE compose down, so the project network
    // (which the relay had joined) goes away cleanly with everything else.
    ASSERT_NO_THROW(compose.stop());
    const auto leftovers = client.list_containers(
        {{"label", "com.docker.compose.project=" + project_name}}, /*all*/ true);
    EXPECT_TRUE(leftovers.empty());
    // A force-removed endpoint can linger for a beat inside libnetwork (and
    // the teardown label sweep covers containers only) — give the network
    // removal the same bounded patience as the PONG above.
    bool network_gone = false;
    const auto net_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < net_deadline) {
        network_gone =
            client.list_networks({{"label", "com.docker.compose.project=" + project_name}}).empty();
        if (network_gone) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    EXPECT_TRUE(network_gone);
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
