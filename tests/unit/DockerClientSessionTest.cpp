#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "CannedHttpServer.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/Timeouts.hpp"

// Tests in this file (DockerClient::Session — scoped keep-alive connection
// reuse, driven against a canned loopback responder; no Docker daemon):
//   Session.ReusesOneConnectionForConsecutiveGets - two GETs inside a session travel over a single connection, without a "Connection: close" header.
//   Session.WithoutSessionEachRequestConnects - the default mode is unchanged: every request opens its own connection and sends "Connection: close".
//   Session.RetriesIdempotentGetOnStaleConnection - a GET hitting a kept-alive connection the server closed while idle FIRST tries the cached connection, then is retried once on a fresh one, returning the REAL response (a stale EOF must not fabricate an empty success).
//   Session.NonGetAlwaysOpensAFreshConnection - non-idempotent requests bypass the cached connection even inside a session (a stale retry can never replay a side effect).
//   Session.EndsAtScopeExit - after the session object is destroyed the client is back to connection-per-request.
//   Session.TimeoutOnReusedConnectionIsNotRetried - an io-deadline expiry on the kept-alive connection surfaces as TransportTimeoutError without a retry (a retry would silently double the io budget).

namespace {

using tcunit::CannedHttpServer;
using tcunit::http_response;

using testcontainers::DockerClient;

std::string ok(const std::string& body) { return http_response(200, "OK", body); }

/// True when the recorded request starts with "<METHOD> <path-prefix>".
bool request_is(const std::string& head, const std::string& method_and_path) {
    return head.rfind(method_and_path, 0) == 0;
}

bool asks_to_close(const std::string& head) {
    return head.find("Connection: close") != std::string::npos;
}

} // namespace

TEST(Session, ReusesOneConnectionForConsecutiveGets) {
    CannedHttpServer server(std::vector<std::vector<std::string>>{{ok("one"), ok("two")}});
    DockerClient client{server.host()};
    const DockerClient::Session session(client);

    EXPECT_EQ(client.request("GET", "/first").body, "one");
    EXPECT_EQ(client.request("GET", "/second").body, "two");

    const auto by_connection = server.requests_by_connection();
    ASSERT_EQ(by_connection.size(), 1u); // both GETs on ONE connection
    ASSERT_EQ(by_connection[0].size(), 2u);
    EXPECT_TRUE(request_is(by_connection[0][0], "GET /first")) << by_connection[0][0];
    EXPECT_TRUE(request_is(by_connection[0][1], "GET /second")) << by_connection[0][1];
    // Keep-alive requests carry no "Connection: close" (the HTTP/1.1 default).
    EXPECT_FALSE(asks_to_close(by_connection[0][0])) << by_connection[0][0];
}

TEST(Session, WithoutSessionEachRequestConnects) {
    CannedHttpServer server({ok("one"), ok("two")});
    DockerClient client{server.host()};

    EXPECT_EQ(client.request("GET", "/first").body, "one");
    EXPECT_EQ(client.request("GET", "/second").body, "two");

    const auto by_connection = server.requests_by_connection();
    ASSERT_EQ(by_connection.size(), 2u); // one connection per request
    EXPECT_TRUE(asks_to_close(by_connection[0][0])) << by_connection[0][0];
    EXPECT_TRUE(asks_to_close(by_connection[1][0])) << by_connection[1][0];
}

TEST(Session, RetriesIdempotentGetOnStaleConnection) {
    // Connection 1 serves ONE real response; the empty second "response" makes
    // the server read (and record) the next request and then close without
    // answering — the podman / proxy "idle keep-alive closed under us"
    // scenario. The second GET must FIRST try the cached connection (proving
    // the session actually reused it) and then transparently retry on a fresh
    // one, returning the REAL second response (not an EOF-fabricated empty
    // success).
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {ok("one"), ""},
        {ok("two")},
    });
    DockerClient client{server.host()};
    const DockerClient::Session session(client);

    EXPECT_EQ(client.request("GET", "/first").body, "one");
    const auto second = client.request("GET", "/second");
    EXPECT_EQ(second.status_code, 200);
    EXPECT_EQ(second.body, "two");

    const auto by_connection = server.requests_by_connection();
    ASSERT_EQ(by_connection.size(), 2u);
    // The stale attempt went over the CACHED connection before the retry.
    ASSERT_EQ(by_connection[0].size(), 2u);
    EXPECT_TRUE(request_is(by_connection[0][1], "GET /second")) << by_connection[0][1];
    ASSERT_EQ(by_connection[1].size(), 1u);
    EXPECT_TRUE(request_is(by_connection[1][0], "GET /second")) << by_connection[1][0];
}

TEST(Session, NonGetAlwaysOpensAFreshConnection) {
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {ok("g1"), ok("g2")}, // connection 1: the session's kept-alive GETs
        {ok("post")},         // connection 2: the POST must NOT reuse conn 1
    });
    DockerClient client{server.host()};
    const DockerClient::Session session(client);

    EXPECT_EQ(client.request("GET", "/a").body, "g1");
    EXPECT_EQ(client.request("GET", "/b").body, "g2");
    EXPECT_EQ(client.request("POST", "/c", "{}").body, "post");

    const auto by_connection = server.requests_by_connection();
    ASSERT_EQ(by_connection.size(), 2u);
    ASSERT_EQ(by_connection[0].size(), 2u);
    ASSERT_EQ(by_connection[1].size(), 1u);
    EXPECT_TRUE(request_is(by_connection[1][0], "POST /c")) << by_connection[1][0];
    // The non-idempotent request behaves exactly as without a session.
    EXPECT_TRUE(asks_to_close(by_connection[1][0])) << by_connection[1][0];
}

namespace {

/// One-connection loopback server for the timeout rule: serves one good
/// keep-alive response, then reads the next request and never answers. The
/// client's io-deadline expiry eventually drops its transport (closing the
/// socket), which unblocks our read with an error and lets the thread exit.
class SilentAfterFirstServer {
public:
    explicit SilentAfterFirstServer(std::string first_response)
        : acceptor_(ioc_, boost::asio::ip::tcp::endpoint(
                              boost::asio::ip::make_address("127.0.0.1"), 0)),
          port_(acceptor_.local_endpoint().port()),
          thread_([this, response = std::move(first_response)] {
              namespace asio = boost::asio;
              boost::system::error_code ec;
              asio::ip::tcp::socket socket(ioc_);
              acceptor_.accept(socket, ec);
              if (ec || stop_) {
                  return;
              }
              char buf[1024];
              std::string request;
              while (request.find("\r\n\r\n") == std::string::npos) {
                  const std::size_t n = socket.read_some(asio::buffer(buf), ec);
                  if (ec) {
                      return;
                  }
                  request.append(buf, n);
              }
              asio::write(socket, asio::buffer(response), ec);
              // Swallow whatever comes next and never answer, until the client
              // gives up and its dropped transport closes the connection.
              while (!ec) {
                  socket.read_some(asio::buffer(buf), ec);
              }
              boost::system::error_code ignore;
              socket.close(ignore);
          }) {}

    ~SilentAfterFirstServer() {
        // Unblock a never-connected accept (same pattern as CannedHttpServer);
        // an in-progress connection is unblocked by the client closing it.
        stop_ = true;
        try {
            {
                boost::asio::io_context poke_io;
                boost::asio::ip::tcp::socket poke(poke_io);
                boost::system::error_code ignore;
                poke.connect(boost::asio::ip::tcp::endpoint(
                                 boost::asio::ip::make_address("127.0.0.1"), port_),
                             ignore);
            }
            thread_.join();
            boost::system::error_code ignore;
            acceptor_.close(ignore);
        } catch (...) {
            // Best-effort: a destructor must never throw (join only throws
            // when the thread is already unjoinable).
        }
    }

    testcontainers::DockerHost host() const {
        return testcontainers::DockerHost::parse("tcp://127.0.0.1:" + std::to_string(port_));
    }

private:
    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace

TEST(Session, TimeoutOnReusedConnectionIsNotRetried) {
    SilentAfterFirstServer server(ok("one"));
    DockerClient client{server.host()};
    testcontainers::docker::TransportTimeouts timeouts;
    timeouts.io = std::chrono::milliseconds(250);
    client.set_transport_timeouts(timeouts);
    const DockerClient::Session session(client);

    EXPECT_EQ(client.request("GET", "/first").body, "one"); // cached afterwards

    // The reused connection stalls: the io deadline must surface as
    // TransportTimeoutError WITHOUT a retry — a retry would also stall (the
    // server never answers again) and the call would take two io budgets.
    const auto start = std::chrono::steady_clock::now();
    EXPECT_THROW(client.request("GET", "/second"), testcontainers::TransportTimeoutError);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_LT(elapsed.count(), 450) << "a retry would have doubled the io budget";
}

TEST(Session, EndsAtScopeExit) {
    CannedHttpServer server({ok("one"), ok("two")});
    DockerClient client{server.host()};

    {
        const DockerClient::Session session(client);
        EXPECT_EQ(client.request("GET", "/first").body, "one");
    } // the session's cached connection is closed here

    const auto second = client.request("GET", "/second");
    EXPECT_EQ(second.body, "two");

    const auto by_connection = server.requests_by_connection();
    ASSERT_EQ(by_connection.size(), 2u);
    // Back to connection-per-request: the post-session GET asks to close.
    EXPECT_TRUE(asks_to_close(by_connection[1][0])) << by_connection[1][0];
}
