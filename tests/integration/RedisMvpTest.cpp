#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <exception>
#include <string>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include "testcontainers/Container.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon):
//   RedisMvp.StartsConnectsAndAutoRemoves - a redis container starts, publishes a host port reachable by a raw TCP PING returning +PONG, and is force-removed once the handle goes out of scope.

using namespace testcontainers;

namespace {

// Send a Redis PING over a raw TCP connection and return the reply (or "").
std::string redis_ping(const std::string& host, std::uint16_t port) {
    namespace asio = boost::asio;
    using asio::ip::tcp;

    asio::io_context io;
    tcp::resolver resolver(io);
    const auto endpoints = resolver.resolve(host, std::to_string(port));

    tcp::socket socket(io);
    asio::connect(socket, endpoints);

    const std::string ping = "PING\r\n";
    asio::write(socket, asio::buffer(ping));

    std::array<char, 64> buf{};
    boost::system::error_code ec;
    const std::size_t n = socket.read_some(asio::buffer(buf), ec);
    return std::string(buf.data(), n);
}

} // namespace

// Requires a reachable Docker daemon; skipped if none is available.
class RedisMvp : public ::testing::Test {
protected:
    void SetUp() override {
        if (auto why = tcit::linux_engine_unavailable()) {
            GTEST_SKIP() << *why;
        }
    }
};

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
