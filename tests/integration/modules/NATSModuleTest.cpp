#include <gtest/gtest.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

#include "testcontainers/modules/NATS.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Linux-containers Docker daemon —
// the default image is FROM scratch, so every assertion is host-side: the
// NATS text protocol over raw TCP, or the HTTP monitoring API):
//   NATSModule.StartsServesAndBuildsUrls - a default NATSImage starts, greets with INFO and answers PING with PONG on host()/port(), /healthz reports ok through monitoring_port(), and url()/monitoring_url() render from the resolved host/ports.
//   NATSModule.AuthIsEnforcedAndWired - with_username/with_password really turn auth on (a bare PING gets Authorization Violation; a CONNECT with the credentials gets PONG) and url() carries user:pass@.
//   NATSModule.JetStreamTurnsOn - with_jetstream reaches the server: the INFO greeting advertises jetstream and /healthz?js-enabled-only=true answers 200.
//   NATSModule.CommandArgsReachTheServer - with_command_args flags reach the running server (the INFO greeting reports the server name set via --name).

using namespace testcontainers;
using modules::NATSContainer;
using modules::NATSImage;

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

/// A raw client connection to a published NATS port. The server speaks a
/// CRLF-line text protocol and greets with `INFO {json}` immediately on
/// connect — the protocol itself is the test driver; no client library.
class NatsConn {
public:
    NatsConn(const std::string& host, std::uint16_t port) : socket_(io_) {
        tcp::resolver resolver(io_);
        asio::connect(socket_, resolver.resolve(host, std::to_string(port)));
    }

    void send(const std::string& data) { asio::write(socket_, asio::buffer(data)); }

    /// Read one CRLF-terminated line, terminator stripped.
    std::string read_line() {
        const std::size_t n = asio::read_until(socket_, asio::dynamic_buffer(buffer_), "\r\n");
        std::string line = buffer_.substr(0, n - 2);
        buffer_.erase(0, n);
        return line;
    }

private:
    asio::io_context io_;
    tcp::socket socket_;
    std::string buffer_;
};

/// Plain HTTP/1.1 GET against a published port; returns the whole response
/// (status line + headers + body).
std::string http_get(const std::string& host, std::uint16_t port, const std::string& target) {
    asio::io_context io;
    tcp::resolver resolver(io);
    tcp::socket socket(io);
    asio::connect(socket, resolver.resolve(host, std::to_string(port)));

    const std::string request =
        "GET " + target + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    asio::write(socket, asio::buffer(request));

    std::string response;
    boost::system::error_code ec;
    asio::read(socket, asio::dynamic_buffer(response), ec); // drain to EOF
    return response;
}

} // namespace

// Requires a Linux-containers daemon; skipped otherwise.
class NATSModule : public tcit::LinuxEngineTest {};

TEST_F(NATSModule, StartsServesAndBuildsUrls) {
    const NATSContainer nats = NATSImage().start();

    NatsConn conn(nats.host(), nats.port());
    EXPECT_EQ(conn.read_line().substr(0, 6), "INFO {");
    conn.send("PING\r\n");
    EXPECT_EQ(conn.read_line(), "PONG");

    EXPECT_EQ(nats.url(), "nats://" + nats.host() + ":" + std::to_string(nats.port()));
    EXPECT_EQ(nats.monitoring_url(),
              "http://" + nats.host() + ":" + std::to_string(nats.monitoring_port()));
    EXPECT_TRUE(nats.username().empty());
    EXPECT_TRUE(nats.password().empty());

    const std::string health = http_get(nats.host(), nats.monitoring_port(), "/healthz");
    EXPECT_EQ(health.substr(0, 12), "HTTP/1.1 200");
    EXPECT_NE(health.find("\"status\":\"ok\""), std::string::npos);
}

TEST_F(NATSModule, AuthIsEnforcedAndWired) {
    const NATSContainer nats = NATSImage().with_username("app").with_password("s3cr3t").start();

    { // Auth is really on: an operation before CONNECT is refused.
        NatsConn bare(nats.host(), nats.port());
        bare.read_line(); // the INFO greeting
        bare.send("PING\r\n");
        EXPECT_NE(bare.read_line().find("Authorization Violation"), std::string::npos);
    }

    // The credentials work: CONNECT then PING answers PONG ("verbose":false
    // suppresses the +OK acknowledgment, so PONG is the next line).
    NatsConn conn(nats.host(), nats.port());
    conn.read_line(); // the INFO greeting
    conn.send("CONNECT {\"verbose\":false,\"user\":\"app\",\"pass\":\"s3cr3t\"}\r\nPING\r\n");
    EXPECT_EQ(conn.read_line(), "PONG");

    EXPECT_NE(nats.url().find("app:s3cr3t@"), std::string::npos);
    EXPECT_EQ(nats.username(), "app");
    EXPECT_EQ(nats.password(), "s3cr3t");
}

TEST_F(NATSModule, JetStreamTurnsOn) {
    const NATSContainer nats = NATSImage().with_jetstream().start();

    NatsConn conn(nats.host(), nats.port());
    EXPECT_NE(conn.read_line().find("\"jetstream\":true"), std::string::npos);

    // The js-enabled-only filter makes /healthz report failure unless
    // JetStream is actually on.
    const std::string health =
        http_get(nats.host(), nats.monitoring_port(), "/healthz?js-enabled-only=true");
    EXPECT_EQ(health.substr(0, 12), "HTTP/1.1 200");
}

TEST_F(NATSModule, CommandArgsReachTheServer) {
    const NATSContainer nats = NATSImage().with_command_args({"--name", "wave2-nats"}).start();

    NatsConn conn(nats.host(), nats.port());
    EXPECT_NE(conn.read_line().find("\"server_name\":\"wave2-nats\""), std::string::npos);
}
