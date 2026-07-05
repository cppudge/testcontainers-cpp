#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "CannedHttpServer.hpp"
#include "docker/ApiMapping.hpp"
#include "testcontainers/docker/DockerClient.hpp"

// Tests in this file (the Docker Engine API version pinning — negotiation is a
// pure helper plus one wire round-trip against a canned loopback responder):
//   ApiVersion.NegotiatesMinOfClientAndDaemon - a daemon newer than the client is pinned down to kClientApiVersion; an older daemon wins with its own version; equal stays equal.
//   ApiVersion.ComparesNumericallyNotLexicographically - "1.9" is treated as OLDER than "1.44" (a lexicographic compare would invert that and pin a 1.9 daemon to 1.44).
//   ApiVersion.UnparsableDaemonVersionMeansNoPin - empty / garbage / partial version strings negotiate to "" (the caller then keeps unversioned paths).
//   ApiVersion.PinsTypedRequestPaths - after the one-time /_ping negotiation, a typed call's path carries the /v1.NN prefix chosen from the daemon's Api-Version header.
//   ApiVersion.OlderDaemonVersionWinsOnTheWire - a daemon reporting 1.41 gets /v1.41/ paths (not the client's newer default).
//   ApiVersion.NoHeaderFallsBackToUnversionedPaths - a daemon whose ping carries no Api-Version header keeps the old unversioned paths.
//   ApiVersion.CopiesInheritTheNegotiatedVersion - a copy of a negotiated client pins the same version WITHOUT issuing a second ping.

using tcunit::CannedHttpServer;
using tcunit::http_response;

using testcontainers::DockerClient;
using testcontainers::docker::kClientApiVersion;
using testcontainers::docker::negotiate_api_version;

namespace {

/// A /_ping reply carrying the daemon's newest supported API version, the way
/// the real daemon's version middleware stamps it.
std::string ping_with_version(const std::string& api_version) {
    return "HTTP/1.1 200 OK\r\nApi-Version: " + api_version +
           "\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nOK";
}

std::string inspect_ok() { return http_response(200, "OK", "{}"); }

/// True when the recorded request head starts with "<METHOD> <path-prefix>".
bool request_is(const std::string& head, const std::string& method_and_path) {
    return head.rfind(method_and_path, 0) == 0;
}

} // namespace

TEST(ApiVersion, NegotiatesMinOfClientAndDaemon) {
    const std::string client{kClientApiVersion};
    EXPECT_EQ(negotiate_api_version("1.51"), client); // daemon newer -> client wins
    EXPECT_EQ(negotiate_api_version("2.0"), client);  // newer major, same rule
    EXPECT_EQ(negotiate_api_version(client), client); // equal stays equal
    EXPECT_EQ(negotiate_api_version("1.41"), "1.41"); // daemon older -> daemon wins
    EXPECT_EQ(negotiate_api_version("1.24"), "1.24");
}

TEST(ApiVersion, ComparesNumericallyNotLexicographically) {
    // "1.9" < "1.44" numerically; a lexicographic compare would call the 1.9
    // daemon NEWER and pin it to a version it does not support.
    EXPECT_EQ(negotiate_api_version("1.9"), "1.9");
    EXPECT_EQ(negotiate_api_version("0.9"), "0.9");
}

TEST(ApiVersion, UnparsableDaemonVersionMeansNoPin) {
    EXPECT_EQ(negotiate_api_version(""), "");
    EXPECT_EQ(negotiate_api_version("latest"), "");
    EXPECT_EQ(negotiate_api_version("1."), "");
    EXPECT_EQ(negotiate_api_version(".44"), "");
    EXPECT_EQ(negotiate_api_version("1.44.2"), "");
    EXPECT_EQ(negotiate_api_version("v1.44"), "");
    EXPECT_EQ(negotiate_api_version("1.4x"), "");
}

TEST(ApiVersion, PinsTypedRequestPaths) {
    CannedHttpServer server({ping_with_version("1.51"), inspect_ok()});
    DockerClient client{server.host()};

    client.inspect_container_raw("abc");

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_TRUE(request_is(requests[0], "GET /_ping")) << requests[0];
    const std::string pinned = "GET /v" + std::string(kClientApiVersion) + "/containers/abc/json";
    EXPECT_TRUE(request_is(requests[1], pinned)) << requests[1];
}

TEST(ApiVersion, OlderDaemonVersionWinsOnTheWire) {
    CannedHttpServer server({ping_with_version("1.41"), inspect_ok()});
    DockerClient client{server.host()};

    client.inspect_container_raw("abc");

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_TRUE(request_is(requests[1], "GET /v1.41/containers/abc/json")) << requests[1];
}

TEST(ApiVersion, NoHeaderFallsBackToUnversionedPaths) {
    CannedHttpServer server({http_response(200, "OK", "OK"), inspect_ok()});
    DockerClient client{server.host()};

    client.inspect_container_raw("abc");

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_TRUE(request_is(requests[1], "GET /containers/abc/json")) << requests[1];
}

TEST(ApiVersion, CopiesInheritTheNegotiatedVersion) {
    CannedHttpServer server({ping_with_version("1.51"), inspect_ok(), inspect_ok()});
    DockerClient client{server.host()};
    client.inspect_container_raw("abc"); // negotiates

    DockerClient copy{client}; // NOLINT(performance-unnecessary-copy-initialization)
    copy.inspect_container_raw("abc");

    // Exactly one ping total: the copy reused the negotiated version.
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 3u);
    const std::string pinned = "GET /v" + std::string(kClientApiVersion) + "/containers/abc/json";
    EXPECT_TRUE(request_is(requests[1], pinned)) << requests[1];
    EXPECT_TRUE(request_is(requests[2], pinned)) << requests[2];
}
