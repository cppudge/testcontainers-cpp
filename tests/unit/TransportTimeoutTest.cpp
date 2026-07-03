#include <gtest/gtest.h>

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "docker/Transport.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerHost.hpp"
#include "testcontainers/docker/Timeouts.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// Tests in this file (loopback servers, no Docker daemon — the deadline
// mechanics of the transport layer):
//   TransportTimeout.ReadTimesOutOnSilentPeer - a read against a connected-but-silent peer fails with timed_out within the io deadline (not never).
//   TransportTimeout.ReadCompletesWhenDataArrivesInTime - data arriving before the deadline is delivered normally (no premature timeout).
//   TransportTimeout.DisabledDeadlineWaitsPastTheOldOne - set_io_timeout(nullopt) waits for data that arrives later than the previously-set deadline.
//   TransportTimeout.SetIoTimeoutAppliesToSubsequentReads - a transport opened without a deadline starts timing out after set_io_timeout(ms).
//   TransportTimeout.WriteTimesOutWhenPeerStopsReading - once the peer's receive window fills, a write fails with timed_out instead of blocking forever.
//   TransportTimeout.ConnectRefusedFailsWithDockerError - connecting to a closed port throws DockerError (the refused path is an error, not a hang).
//   TransportTimeout.NamedPipeReadTimesOutOnSilentServer - (Windows) a named-pipe read against a silent pipe server fails with timed_out within the deadline.

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;
using namespace std::chrono_literals;

using testcontainers::DockerError;
using testcontainers::DockerHost;
using testcontainers::docker::TransportTimeouts;

/// A loopback TCP server accepting ONE connection, running `session` on it,
/// then holding the socket open (silently) until the server is destroyed —
/// the shape a wedged daemon presents: connected, but never sends a byte.
class LoopbackServer {
public:
    using Session = std::function<void(tcp::socket&)>;

    explicit LoopbackServer(Session session = {})
        : acceptor_(ioc_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          port_(acceptor_.local_endpoint().port()),
          thread_([this, session = std::move(session)] {
              boost::system::error_code ec;
              tcp::socket socket(ioc_);
              acceptor_.accept(socket, ec);
              if (ec) {
                  return; // destroyed before a client connected
              }
              if (session) {
                  session(socket);
              }
              stop_.get_future().wait(); // hold the connection open, silently
              boost::system::error_code ignore;
              socket.close(ignore);
          }) {}

    ~LoopbackServer() {
        stop_.set_value();
        boost::system::error_code ignore;
        acceptor_.close(ignore); // unblock accept() if no client ever connected
        thread_.join();
    }

    std::uint16_t port() const noexcept { return port_; }

    DockerHost host() const {
        return DockerHost::parse("tcp://127.0.0.1:" + std::to_string(port_));
    }

private:
    asio::io_context ioc_;
    tcp::acceptor acceptor_;
    std::uint16_t port_;
    std::promise<void> stop_;
    std::thread thread_;
};

std::chrono::milliseconds elapsed_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
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
    const std::vector<char> chunk(64 * 1024, 'x');
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
                EXPECT_NE(std::string(e.what()).find("Cannot connect to Docker"),
                          std::string::npos)
                    << e.what();
                throw;
            }
        },
        DockerError);
}

#if defined(_WIN32)

TEST(TransportTimeout, NamedPipeReadTimesOutOnSilentServer) {
    // A local named-pipe server that accepts the connection and never writes —
    // the primary Windows transport must time the read out, not hang.
    const std::string pipe_name =
        R"(\\.\pipe\tc-timeout-test-)" + std::to_string(::GetCurrentProcessId());
    const HANDLE pipe =
        ::CreateNamedPipeA(pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                           /*instances*/ 1, /*out buf*/ 4096, /*in buf*/ 4096,
                           /*default timeout*/ 0, /*security*/ nullptr);
    ASSERT_NE(pipe, INVALID_HANDLE_VALUE) << "CreateNamedPipeA: " << ::GetLastError();

    std::promise<void> stop;
    std::thread server([&] {
        ::ConnectNamedPipe(pipe, nullptr); // blocks until the client connects
        stop.get_future().wait();          // then hold the pipe open, silently
    });

    TransportTimeouts timeouts;
    timeouts.io = 250ms;
    // DockerHost::parse keeps the npipe path with forward slashes; the
    // transport converts back to backslashes.
    const DockerHost host = DockerHost::parse(
        "npipe:////./pipe/tc-timeout-test-" + std::to_string(::GetCurrentProcessId()));
    const auto transport = testcontainers::docker::connect(host, timeouts);

    char byte = 0;
    boost::system::error_code ec;
    const auto start = std::chrono::steady_clock::now();
    const std::size_t n = transport->read_some(&byte, 1, ec);

    EXPECT_EQ(n, 0u);
    EXPECT_EQ(ec, asio::error::timed_out) << ec.message();
    EXPECT_LT(elapsed_since(start), 5s);

    transport->close();
    stop.set_value();
    server.join();
    ::CloseHandle(pipe);
}

#endif // _WIN32
