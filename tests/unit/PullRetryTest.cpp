#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "CannedHttpServer.hpp"
#include "TestSupport.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/RegistryAuth.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/DockerHost.hpp"

// Tests in this file (pull_image's bounded retry on daemon 5xx — no Docker
// daemon; a canned loopback responder plays the daemon relaying registry
// trouble):
//   PullRetry.DefaultPolicyAndCopySemantics - the shipped policy is 3 total tries starting at a 1s backoff; copies carry a tuned policy along; attempts<1 clamps to 1.
//   PullRetry.RetriesOn5xxThenSucceeds - a 500 followed by a clean 200 progress stream succeeds silently (two pull attempts on the wire).
//   PullRetry.GivesUpAfterConfiguredAttempts - three straight 5xx exhaust attempts=3; the LAST status surfaces on the DockerError with the image as resource_id.
//   PullRetry.DoesNotRetry404 - a 404 is permanent: NotFoundError after exactly one pull attempt (the canned tripwire response stays unserved).
//   PullRetry.InStreamErrorIsNotRetried - an error embedded in the 200 progress stream (how most daemons report a bad image name) fails after one attempt.

namespace {

using tcunit::CannedHttpServer;
using tcunit::http_response;
using tcunit::ping_ok;
using tcunit::request_is;

using testcontainers::DockerClient;
using testcontainers::DockerError;
using testcontainers::NotFoundError;
using testcontainers::RegistryAuth;

/// A retry policy whose backoff doesn't slow the suite down.
DockerClient::PullRetry fast_retry(int attempts) {
    return {attempts, std::chrono::milliseconds(1)};
}

// Every pull below passes explicit (empty) credentials so the pull path stays
// off this host's Docker config / credential helpers — a unit test must not
// shell out.

} // namespace

TEST(PullRetry, DefaultPolicyAndCopySemantics) {
    // Pin the shipped policy: changing it is a behavior change users see.
    DockerClient client{testcontainers::DockerHost::parse("tcp://127.0.0.1:1")};
    EXPECT_EQ(client.pull_retry().attempts, 3);
    EXPECT_EQ(client.pull_retry().first_delay, std::chrono::seconds(1));

    client.set_pull_retry({5, std::chrono::milliseconds(10)});
    const DockerClient copy = client;
    EXPECT_EQ(copy.pull_retry().attempts, 5);
    EXPECT_EQ(copy.pull_retry().first_delay, std::chrono::milliseconds(10));

    // attempts < 1 clamps to a single try instead of "never try".
    client.set_pull_retry({0, std::chrono::milliseconds(1)});
    EXPECT_EQ(client.pull_retry().attempts, 1);
}

TEST(PullRetry, RetriesOn5xxThenSucceeds) {
    CannedHttpServer server(std::vector<std::string>{
        // 0) GET /_ping -> the one-time API-version negotiation
        ping_ok(),
        // 1) POST /images/create -> the daemon relaying a registry blip
        http_response(500, "Internal Server Error",
                      R"({"message":"received unexpected HTTP status: 500"})"),
        // 2) POST /images/create retry -> a clean progress stream
        http_response(200, "OK", "{\"status\":\"Pulling from library/busybox\"}\n"),
    });
    DockerClient client{server.host()};
    client.set_pull_retry(fast_retry(3));

    client.pull_image("busybox:latest", RegistryAuth{});

    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 3u); // ping + 2 pull attempts
    EXPECT_TRUE(request_is(requests[1], "POST /images/create")) << requests[1];
    EXPECT_TRUE(request_is(requests[2], "POST /images/create")) << requests[2];
}

TEST(PullRetry, GivesUpAfterConfiguredAttempts) {
    CannedHttpServer server(std::vector<std::string>{
        ping_ok(),
        http_response(500, "Internal Server Error", R"({"message":"blip 1"})"),
        http_response(502, "Bad Gateway", R"({"message":"blip 2"})"),
        http_response(503, "Service Unavailable", R"({"message":"blip 3"})"),
    });
    DockerClient client{server.host()};
    client.set_pull_retry(fast_retry(3));

    try {
        client.pull_image("busybox:latest", RegistryAuth{});
        FAIL() << "expected DockerError";
    } catch (const DockerError& e) {
        EXPECT_EQ(e.status_code(), 503); // the LAST attempt's status surfaces
        EXPECT_EQ(e.resource_id(), "busybox:latest");
        EXPECT_NE(std::string(e.what()).find("pull_image"), std::string::npos) << e.what();
    }
    EXPECT_EQ(server.requests().size(), 4u); // ping + 3 attempts, then give up
}

TEST(PullRetry, DoesNotRetry404) {
    // The trailing tripwire response only gets served if pull_image WRONGLY
    // retries a permanent status.
    CannedHttpServer server(std::vector<std::string>{
        ping_ok(),
        http_response(404, "Not Found", R"({"message":"no such image"})"),
        http_response(500, "Internal Server Error", R"({"message":"tripwire"})"),
    });
    DockerClient client{server.host()};
    client.set_pull_retry(fast_retry(3));

    EXPECT_THROW(client.pull_image("busybox:latest", RegistryAuth{}), NotFoundError);
    EXPECT_EQ(server.requests().size(), 2u); // ping + the one pull attempt
}

TEST(PullRetry, InStreamErrorIsNotRetried) {
    // Most daemons report a nonexistent image through a 200 as an in-stream
    // "error" line — retrying that would only delay the most common failure,
    // so it must surface immediately.
    CannedHttpServer server(std::vector<std::string>{
        ping_ok(),
        http_response(200, "OK", "{\"status\":\"Pulling\"}\n{\"error\":\"pull access denied\"}\n"),
        http_response(200, "OK", "{\"status\":\"tripwire\"}\n"),
    });
    DockerClient client{server.host()};
    client.set_pull_retry(fast_retry(3));

    EXPECT_THROW(client.pull_image("nope:latest", RegistryAuth{}), DockerError);
    EXPECT_EQ(server.requests().size(), 2u);
}
