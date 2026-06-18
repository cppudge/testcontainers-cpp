#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/DockerComposeContainer.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Linux Docker daemon):
//   Compose.BringsUpAndExposesRedis - a compose project run through the container-based ambassador brings up redis, the published host port answers a raw TCP PING with PONG, and stop() removes every container carrying the project label.

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

// Requires a reachable Linux Docker daemon; skipped otherwise.
class Compose : public ::testing::Test {
protected:
    void SetUp() override {
        if (auto why = tcit::linux_engine_unavailable()) {
            GTEST_SKIP() << *why;
        }
    }
};

TEST_F(Compose, BringsUpAndExposesRedis) {
    // A valid compose file publishing redis 6379 to a random host port. The
    // ambassador (docker/compose image) does the orchestration against the host
    // daemon; we only ship it this file.
    const std::string yaml = R"(services:
  redis:
    image: redis:7.2
    ports:
      - "6379"
)";

    std::string project_name;

    {
        DockerComposeContainer compose =
            DockerComposeContainer::from_yaml(yaml).with_exposed_service("redis", tcp(6379));
        project_name = compose.project_name();

        // Brings the stack up via the ambassador and waits for the published
        // redis host port to accept a connection. May pull docker/compose and the
        // redis image the first time — allow generous time.
        ASSERT_NO_THROW(compose.start());

        const std::uint16_t host_port = compose.get_service_port("redis", tcp(6379));
        EXPECT_GT(host_port, 0);
        const std::string host = compose.get_service_host("redis");

        // The published host port must answer a Redis PING with +PONG.
        std::string reply;
        ASSERT_NO_THROW(reply = redis_ping(host, host_port));
        EXPECT_NE(reply.find("PONG"), std::string::npos);

        // Tearing down removes every container carrying the project label.
        ASSERT_NO_THROW(compose.stop());

        DockerClient client = DockerClient::from_environment();
        const auto leftovers = client.list_containers(
            {{"label", "com.docker.compose.project=" + project_name}}, /*all*/ true);
        EXPECT_TRUE(leftovers.empty());
    } // The DockerComposeContainer destructor is a no-op here (already stopped).
}
