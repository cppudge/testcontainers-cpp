#include <gtest/gtest.h>

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "LoopbackServer.hpp"
#include "TestSupport.hpp"
#include "docker/Transport.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/DockerHost.hpp"
#include "testcontainers/docker/Timeouts.hpp"

#if defined(_WIN32)
#include "PipeServer.hpp"
#endif

// Tests in this file (loopback servers, no Docker daemon — the deadline
// mechanics of the transport layer):
//   TransportTimeout.ReadTimesOutOnSilentPeer - a read against a connected-but-silent peer fails with timed_out within the io deadline (not never).
//   TransportTimeout.ReadCompletesWhenDataArrivesInTime - data arriving before the deadline is delivered normally (no premature timeout).
//   TransportTimeout.DisabledDeadlineWaitsPastTheOldOne - set_io_timeout(nullopt) waits for data that arrives later than the previously-set deadline.
//   TransportTimeout.SetIoTimeoutAppliesToSubsequentReads - a transport opened without a deadline starts timing out after set_io_timeout(ms).
//   TransportTimeout.WriteTimesOutWhenPeerStopsReading - once the peer's receive window fills, a write fails with timed_out instead of blocking forever.
//   TransportTimeout.ConnectRefusedFailsWithDockerError - connecting to a closed port throws DockerError (the refused path is an error, not a hang).
//   TransportTimeout.TlsHandshakeTimesOutOnSilentPeer - [TC_TLS builds] a TLS handshake against a peer that never answers the ClientHello throws TransportTimeoutError within the connect budget (composed-op cancellation).
//   TransportTimeout.TlsDisabledConnectThrowsNamedError - [TC_TLS=OFF builds] connect() for an https:// host throws DockerError naming the TC_TLS option instead of dialing at all.
//   TransportTimeout.RequestTimesOutMidBody - DockerClient::request against a daemon that stalls mid-body throws TransportTimeoutError (status_code()==nullopt) instead of hanging the Beast parser loop (end-to-end through TransportStream).
//   TransportTimeout.NamedPipeReadTimesOutOnSilentServer - (Windows) a named-pipe read against a silent pipe server fails with timed_out within the deadline.
//   TransportTimeout.NamedPipeRequestThrowsTypedTimeout - (Windows) DockerClient::request over a silent named pipe throws TransportTimeoutError - the typed path on the primary Windows transport.

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;
using namespace std::chrono_literals;

using testcontainers::DockerError;
using testcontainers::DockerHost;
using testcontainers::TransportTimeoutError;
using testcontainers::docker::TransportTimeouts;

// The wedged-daemon shape every test here needs — accept, then hold the
// socket silently — is the shared tcunit::LoopbackServer's default.
using tcunit::LoopbackServer;

std::chrono::milliseconds elapsed_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start);
}

} // namespace

TEST(TransportTimeout, ReadTimesOutOnSilentPeer) {
    LoopbackServer server; // accepts, then stays silent
    TransportTimeouts timeouts;
    timeouts.io = 250ms;
    const auto transport = testcontainers::docker::connect(server.host(), timeouts);

    char byte = 0;
    boost::system::error_code ec;
    const auto start = std::chrono::steady_clock::now();
    const std::size_t n = transport->read_some(&byte, 1, ec);

    EXPECT_EQ(n, 0u);
    EXPECT_EQ(ec, asio::error::timed_out) << ec.message();
    // The whole point: bounded. Well under the "forever" this used to be, and
    // not an instant spurious failure either (allow generous scheduler slop).
    EXPECT_GE(elapsed_since(start), 150ms);
    EXPECT_LT(elapsed_since(start), 5s);
}

TEST(TransportTimeout, ReadCompletesWhenDataArrivesInTime) {
    LoopbackServer server([](tcp::socket& socket) {
        std::this_thread::sleep_for(100ms);
        asio::write(socket, asio::buffer(std::string("ping")));
    });
    TransportTimeouts timeouts;
    timeouts.io = 5s;
    const auto transport = testcontainers::docker::connect(server.host(), timeouts);

    char buf[16] = {};
    boost::system::error_code ec;
    const std::size_t n = transport->read_some(buf, sizeof(buf), ec);

    ASSERT_FALSE(ec) << ec.message();
    EXPECT_GT(n, 0u);
    EXPECT_EQ(std::string(buf, n).substr(0, 4), "ping");
}

TEST(TransportTimeout, DisabledDeadlineWaitsPastTheOldOne) {
    LoopbackServer server([](tcp::socket& socket) {
        std::this_thread::sleep_for(400ms); // longer than the initial deadline
        asio::write(socket, asio::buffer(std::string("late")));
    });
    TransportTimeouts timeouts;
    timeouts.io = 150ms;
    const auto transport = testcontainers::docker::connect(server.host(), timeouts);
    transport->set_io_timeout(std::nullopt); // the streaming call sites' switch

    char buf[16] = {};
    boost::system::error_code ec;
    const std::size_t n = transport->read_some(buf, sizeof(buf), ec);

    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(std::string(buf, n).substr(0, 4), "late");
}

TEST(TransportTimeout, SetIoTimeoutAppliesToSubsequentReads) {
    LoopbackServer server([](tcp::socket& socket) {
        asio::write(socket, asio::buffer(std::string("hi")));
        // ... then silence (the fixture holds the socket open).
    });
    TransportTimeouts timeouts;
    timeouts.io = std::nullopt; // start without a deadline
    const auto transport = testcontainers::docker::connect(server.host(), timeouts);

    char buf[16] = {};
    boost::system::error_code ec;
    const std::size_t n = transport->read_some(buf, sizeof(buf), ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_GT(n, 0u);

    transport->set_io_timeout(200ms);
    const std::size_t n2 = transport->read_some(buf, sizeof(buf), ec);
    EXPECT_EQ(n2, 0u);
    EXPECT_EQ(ec, asio::error::timed_out) << ec.message();
}

TEST(TransportTimeout, WriteTimesOutWhenPeerStopsReading) {
    LoopbackServer server; // accepts and never reads
    TransportTimeouts timeouts;
    timeouts.io = 250ms;
    const auto transport = testcontainers::docker::connect(server.host(), timeouts);

    // Keep writing until the peer's receive window + our send buffer fill and
    // the write can no longer make progress. Bounded loop: if 256 MiB "sends"
    // without ever blocking, something is very wrong with the fixture.
    const std::vector<char> chunk(std::size_t{64} * 1024, 'x');
    boost::system::error_code ec;
    for (int i = 0; i < 4096 && !ec; ++i) {
        transport->write_some(chunk.data(), chunk.size(), ec);
    }
    EXPECT_EQ(ec, asio::error::timed_out) << ec.message();
}

TEST(TransportTimeout, ConnectRefusedFailsWithDockerError) {
    // Grab a free port, then close the listener so nothing accepts on it.
    std::uint16_t free_port = 0;
    {
        asio::io_context ioc;
        tcp::acceptor acceptor(ioc, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        free_port = acceptor.local_endpoint().port();
    }
    const DockerHost host = DockerHost::parse("tcp://127.0.0.1:" + std::to_string(free_port));

    TransportTimeouts timeouts;
    timeouts.connect = 3s;
    EXPECT_THROW(
        {
            try {
                testcontainers::docker::connect(host, timeouts);
            } catch (const DockerError& e) {
                EXPECT_NE(std::string(e.what()).find("Cannot connect to Docker"), std::string::npos)
                    << e.what();
                throw;
            }
        },
        DockerError);
}

#if defined(TC_TLS)
TEST(TransportTimeout, TlsHandshakeTimesOutOnSilentPeer) {
    LoopbackServer server; // accepts, never answers the ClientHello
    TransportTimeouts timeouts;
    timeouts.connect = 500ms;
    const DockerHost host = DockerHost::parse("https://127.0.0.1:" + std::to_string(server.port()));

    const auto start = std::chrono::steady_clock::now();
    try {
        testcontainers::docker::connect(host, timeouts);
        FAIL() << "expected TransportTimeoutError";
    } catch (const TransportTimeoutError& e) {
        // The typed timeout is also a DockerError (checked by the static
        // hierarchy tests); here the specific type must survive the throw.
        EXPECT_NE(std::string(e.what()).find("TLS handshake"), std::string::npos) << e.what();
    }
    EXPECT_LT(elapsed_since(start), 5s);
}
#else
TEST(TransportTimeout, TlsDisabledConnectThrowsNamedError) {
    // No handshake to deadline in a TLS-less build: the Https branch must
    // refuse up front — before dialing — with the option spelled out. This
    // also runs in `conan create -o tls=False` (unit suite only there).
    LoopbackServer server; // never actually contacted
    const DockerHost host = DockerHost::parse("https://127.0.0.1:" + std::to_string(server.port()));
    try {
        testcontainers::docker::connect(host);
        FAIL() << "expected DockerError (this build has TC_TLS=OFF)";
    } catch (const DockerError& e) {
        EXPECT_NE(std::string(e.what()).find("TC_TLS"), std::string::npos) << e.what();
    }
}
#endif // TC_TLS

TEST(TransportTimeout, RequestTimesOutMidBody) {
    LoopbackServer server([](tcp::socket& socket) {
        // A response header promising more body than is ever sent — a daemon
        // that wedges mid-response.
        asio::write(socket, asio::buffer(std::string(
                                "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npartial")));
        // ... then silence (the fixture holds the socket open).
    });

    testcontainers::DockerClient client{server.host()};
    TransportTimeouts timeouts;
    timeouts.io = 250ms;
    client.set_transport_timeouts(timeouts);

    const auto start = std::chrono::steady_clock::now();
    try {
        client.request("GET", "/wedged");
        FAIL() << "expected TransportTimeoutError";
    } catch (const TransportTimeoutError& e) {
        EXPECT_NE(std::string(e.what()).find("Failed to read response"), std::string::npos)
            << e.what();
        EXPECT_EQ(e.status_code(), std::nullopt); // a timeout never saw a status
    }
    EXPECT_LT(elapsed_since(start), 5s);
}

#if defined(_WIN32)

TEST(TransportTimeout, NamedPipeReadTimesOutOnSilentServer) {
    // A local named-pipe server that accepts the connection and never writes —
    // the primary Windows transport must time the read out, not hang.
    tcunit::PipeServer server("tc-timeout-test", tcunit::PipeServer::Mode::Byte);
    ASSERT_TRUE(server.valid()) << "CreateNamedPipeA: " << ::GetLastError();

    TransportTimeouts timeouts;
    timeouts.io = 250ms;
    const auto transport = testcontainers::docker::connect(server.host(), timeouts);

    char byte = 0;
    boost::system::error_code ec;
    const auto start = std::chrono::steady_clock::now();
    const std::size_t n = transport->read_some(&byte, 1, ec);

    EXPECT_EQ(n, 0u);
    EXPECT_EQ(ec, asio::error::timed_out) << ec.message();
    EXPECT_LT(elapsed_since(start), 5s);

    transport->close();
}

TEST(TransportTimeout, NamedPipeRequestThrowsTypedTimeout) {
    // Same silent-pipe-server shape, driven through DockerClient::request: the
    // request is written (fits the pipe buffer), the response never comes, and
    // the typed TransportTimeoutError must survive to the caller on the
    // primary Windows transport.
    tcunit::PipeServer server("tc-timeout-req-test", tcunit::PipeServer::Mode::Byte);
    ASSERT_TRUE(server.valid()) << "CreateNamedPipeA: " << ::GetLastError();

    testcontainers::DockerClient client{server.host()};
    TransportTimeouts timeouts;
    timeouts.io = 250ms;
    client.set_transport_timeouts(timeouts);

    const auto start = std::chrono::steady_clock::now();
    try {
        client.request("GET", "/wedged");
        FAIL() << "expected TransportTimeoutError";
    } catch (const TransportTimeoutError& e) {
        EXPECT_EQ(e.status_code(), std::nullopt) << e.what();
    }
    EXPECT_LT(elapsed_since(start), 5s);
}

#endif // _WIN32
