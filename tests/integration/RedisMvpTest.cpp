#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"
#include "RedisPing.hpp"

// Tests in this file (integration; require a Docker daemon):
//   RedisMvp.StartsConnectsAndAutoRemoves - a redis container starts, publishes a host port reachable by a raw TCP PING returning +PONG, and is force-removed once the handle goes out of scope.

using namespace testcontainers;

using tcit::redis_ping;

// Requires a reachable Docker daemon; skipped if none is available.
class RedisMvp : public tcit::LinuxEngineTest {};

TEST_F(RedisMvp, StartsConnectsAndAutoRemoves) {
    std::string container_id;

    {
        Container redis = GenericImage("redis", "7.2")
                              .with_exposed_port(tcp(6379))
                              .with_wait(wait_for::stdout_message("Ready to accept connections"))
                              .start();

        container_id = redis.id();
        ASSERT_FALSE(container_id.empty());

        EXPECT_TRUE(redis.is_running());

        const std::uint16_t host_port = redis.get_host_port(tcp(6379));
        EXPECT_GT(host_port, 0);

        // A raw TCP connection to the published host port must succeed, and the
        // server must answer a Redis PING with +PONG.
        std::string reply;
        ASSERT_NO_THROW(reply = redis_ping(redis.host(), host_port));
        EXPECT_EQ(reply.substr(0, 5), "+PONG");
    } // Container destructor force-removes the container here.

    // After teardown, inspecting the (now-removed) container must fail.
    DockerClient client = DockerClient::from_environment();
    EXPECT_THROW(client.inspect_container(container_id), DockerError);
}
