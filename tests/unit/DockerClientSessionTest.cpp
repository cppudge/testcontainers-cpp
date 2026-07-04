#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "CannedHttpServer.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerClient.hpp"

// Tests in this file (DockerClient::Session — scoped keep-alive connection
// reuse, driven against a canned loopback responder; no Docker daemon):
//   Session.ReusesOneConnectionForConsecutiveGets - two GETs inside a session travel over a single connection, without a "Connection: close" header.
//   Session.WithoutSessionEachRequestConnects - the default mode is unchanged: every request opens its own connection and sends "Connection: close".
//   Session.RetriesIdempotentGetOnStaleConnection - a GET hitting a kept-alive connection the server closed while idle is retried once on a fresh connection, returning the REAL response (a stale EOF must not fabricate an empty success).
//   Session.NonGetAlwaysOpensAFreshConnection - non-idempotent requests bypass the cached connection even inside a session (a stale retry can never replay a side effect).
//   Session.EndsAtScopeExit - after the session object is destroyed the client is back to connection-per-request.

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
    // Connection 1 serves ONE response and is then closed by the server — the
    // podman / proxy "idle keep-alive closed under us" scenario. The second
    // GET finds the cached connection dead and must transparently retry on a
    // fresh one, returning the REAL second response (not an EOF-fabricated
    // empty success).
    CannedHttpServer server({ok("one"), ok("two")});
    DockerClient client{server.host()};
    const DockerClient::Session session(client);

    EXPECT_EQ(client.request("GET", "/first").body, "one");
    const auto second = client.request("GET", "/second");
    EXPECT_EQ(second.status_code, 200);
    EXPECT_EQ(second.body, "two");

    const auto by_connection = server.requests_by_connection();
    ASSERT_EQ(by_connection.size(), 2u);
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
