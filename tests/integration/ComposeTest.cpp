#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
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
//   Compose.LocalClientBringsUpRedis - the LOCAL client (host `docker compose` CLI; default) brings up redis from a temp YAML, the published host port answers a raw TCP PING with PONG, and stop() removes every container carrying the project label. Skipped if the host has no `docker compose`.
//   Compose.ContainerisedClientBringsUpRedis - the CONTAINERISED client (long-lived docker:26.1-cli + exec) brings up redis (pulling docker:26.1-cli on first run), PING/PONG succeeds, and teardown leaves nothing.
//   Compose.AutoClientBringsUpRedis - the AUTO client (local first, else containerised) brings up redis, PING/PONG succeeds, and teardown leaves nothing.

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
        if (auto why = tcit::linux_engine_unavailable()) {
            GTEST_SKIP() << *why;
        }
    }
};

TEST_F(Compose, LocalClientBringsUpRedis) {
    if (!host_docker_compose_available()) {
        GTEST_SKIP() << "host `docker compose` CLI is not available; "
                        "local-client test cannot run";
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
