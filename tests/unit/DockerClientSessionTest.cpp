#include <gtest/gtest.h>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "CannedHttpServer.hpp"
#include "LoopbackServer.hpp"
#include "TestSupport.hpp"
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
using tcunit::request_is;

using testcontainers::DockerClient;

std::string ok(const std::string& body) { return http_response(200, "OK", body); }

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

/// Session for the timeout rule: serve one good keep-alive response, then
/// swallow whatever comes next and never answer — the client's io-deadline
/// expiry eventually drops its transport (closing the connection), which
/// errors our read and lets the server thread exit into its hold.
tcunit::LoopbackServer::Session silent_after_first(std::string first_response) {
    return [response = std::move(first_response)](boost::asio::ip::tcp::socket& socket) {
        namespace asio = boost::asio;
        boost::system::error_code ec;
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
        while (!ec) {
            socket.read_some(asio::buffer(buf), ec);
        }
    };
}

} // namespace

TEST(Session, TimeoutOnReusedConnectionIsNotRetried) {
    tcunit::LoopbackServer server(silent_after_first(ok("one")));
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
