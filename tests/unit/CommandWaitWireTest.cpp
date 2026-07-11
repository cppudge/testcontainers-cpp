#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "CannedHttpServer.hpp"
#include "TestSupport.hpp"
#include "WaitStrategies.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/Timeouts.hpp"

// Tests in this file (the wait_for::Command strategy driven through
// detail::wait_until_ready against a canned loopback responder; no Docker
// daemon):
//   CommandWait.SucceedsOnExitZero - one attempt: exec create/start/inspect run, exit code 0 satisfies the wait; the create body carries the argv and the inspect GET reuses the polling session connection (which the ping opened).
//   CommandWait.RetriesUntilExitZero - an attempt exiting 1 is retried after the poll interval; the second attempt's exit 0 satisfies the wait (two exec creates, all inspects on the one session connection).
//   CommandWait.TimeoutReportsLastCompletedAttempt - the timeout error carries the last COMPLETED attempt's exit code and output snippet: the final attempt, cut off by the expired deadline (its inspect still says Running), must not mask them.
//   CommandWait.DaemonErrorIsRetriedUntilSuccess - a 409 from the exec create ("container is not running" — e.g. still restarting) is "not ready yet": the wait retries and succeeds on the next attempt.
//   CommandWait.NotFoundPropagates - a 404 from the exec create means the container is gone for good: NotFoundError propagates instead of being retried into a timeout.
//   CommandWait.EmptyCommandThrows - an empty argv is rejected with DockerError up front, before any daemon interaction.

namespace {

using tcunit::CannedHttpServer;
using tcunit::contains;
using tcunit::created;
using tcunit::frame;
using tcunit::http_response;
using tcunit::ping_ok;
using tcunit::request_is;

using testcontainers::DockerClient;
using testcontainers::DockerError;
using testcontainers::NotFoundError;
using testcontainers::StartupTimeoutError;
using testcontainers::WaitFor;

std::string exec_inspect_exit(int code) {
    return http_response(200, "OK", R"({"Running":false,"ExitCode":)" + std::to_string(code) + "}");
}

std::string exec_inspect_running() {
    return http_response(200, "OK", R"({"Running":true,"ExitCode":null})");
}

/// A client with a short io deadline so a regression fails in seconds, not
/// the 60s default.
DockerClient fast_client(const CannedHttpServer& server) {
    DockerClient client{server.host()};
    testcontainers::docker::TransportTimeouts timeouts;
    timeouts.io = std::chrono::seconds(2);
    client.set_transport_timeouts(timeouts);
    return client;
}

// Connection accept order per attempt: exec connects its hijack transport
// FIRST (its start request arrives only after the create round-trip), then
// the polling session connection opens on the first GET (the
// version-negotiation ping) and stays for every exec-inspect GET, then each
// exec create POST takes a one-shot connection of its own.

} // namespace

TEST(CommandWait, SucceedsOnExitZero) {
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {http_response(200, "OK", frame(1, "PONG"))}, // exec start (200 fallback)
        {ping_ok(), exec_inspect_exit(0)},            // session: ping + exec inspect
        {created("e1")},                              // exec create
    });
    DockerClient client = fast_client(server);

    testcontainers::detail::wait_until_ready(
        client, "abc", {testcontainers::wait_for::successful_command({"redis-cli", "ping"})},
        std::chrono::seconds(5));

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 4u);
    EXPECT_TRUE(request_is(requests[0], "GET /_ping")) << requests[0];
    EXPECT_TRUE(request_is(requests[1], "POST /containers/abc/exec")) << requests[1];
    EXPECT_TRUE(contains(requests[1], "redis-cli")) << requests[1];
    EXPECT_TRUE(request_is(requests[2], "POST /exec/e1/start")) << requests[2];
    EXPECT_TRUE(request_is(requests[3], "GET /exec/e1/json")) << requests[3];

    // The ping and the exec inspect shared the polling session connection.
    const auto by_connection = server.requests_by_connection();
    ASSERT_GE(by_connection.size(), 2u);
    EXPECT_EQ(by_connection[1].size(), 2u);
}

TEST(CommandWait, RetriesUntilExitZero) {
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {http_response(200, "OK", frame(1, "not yet"))},         // attempt-1 start
        {ping_ok(), exec_inspect_exit(1), exec_inspect_exit(0)}, // session
        {created("e1")},                                         // attempt-1 create
        {http_response(200, "OK", frame(1, "ready"))},           // attempt-2 start
        {created("e2")},                                         // attempt-2 create
    });
    DockerClient client = fast_client(server);

    testcontainers::wait_for::Command cond;
    cond.cmd = {"pg_isready"};
    cond.poll_interval = std::chrono::milliseconds(50);
    testcontainers::detail::wait_until_ready(client, "abc", {WaitFor{cond}},
                                             std::chrono::seconds(10));

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 7u); // ping + 2 x (create, start, inspect)
    EXPECT_TRUE(request_is(requests[4], "POST /containers/abc/exec")) << requests[4];
    EXPECT_TRUE(request_is(requests[5], "POST /exec/e2/start")) << requests[5];

    // Both inspects (and the ping) rode the one session connection.
    const auto by_connection = server.requests_by_connection();
    ASSERT_GE(by_connection.size(), 2u);
    EXPECT_EQ(by_connection[1].size(), 3u);
}

TEST(CommandWait, TimeoutReportsLastCompletedAttempt) {
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {http_response(200, "OK", frame(1, "waiting for server"))}, // attempt-1 start
        {ping_ok(), exec_inspect_exit(1), exec_inspect_running()},  // session
        {created("e1")},                                            // attempt-1 create
        {http_response(200, "OK", "")}, // attempt-2 start (body never read)
        {created("e2")},                // attempt-2 create
    });
    DockerClient client = fast_client(server);

    // Attempt 1 completes well inside the budget (exit 1, output captured);
    // the poll interval then sleeps to the deadline, so attempt 2 starts
    // already expired: it is cut off (no body read, inspect says Running) and
    // must NOT mask attempt 1's informative outcome in the error message.
    testcontainers::wait_for::Command cond;
    cond.cmd = {"pg_isready"};
    cond.poll_interval = std::chrono::seconds(10);
    try {
        testcontainers::detail::wait_until_ready(client, "abc", {WaitFor{cond}},
                                                 std::chrono::milliseconds(1500));
        FAIL() << "expected StartupTimeoutError";
    } catch (const StartupTimeoutError& e) {
        EXPECT_EQ(e.resource_id(), "abc");
        const std::string what = e.what();
        EXPECT_TRUE(contains(what, "Timed out waiting for command \"pg_isready\"")) << what;
        EXPECT_TRUE(contains(what, "last exit code 1")) << what;
        EXPECT_TRUE(contains(what, "waiting for server")) << what;
    }
}

TEST(CommandWait, DaemonErrorIsRetriedUntilSuccess) {
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {},                                // attempt-1 hijack, abandoned when the create fails
        {ping_ok(), exec_inspect_exit(0)}, // session: ping + attempt-2 inspect
        {http_response(409, "Conflict", R"({"message":"container abc is not running"})")},
        {http_response(200, "OK", frame(1, "ok"))}, // attempt-2 start
        {created("e2")},                            // attempt-2 create
    });
    DockerClient client = fast_client(server);

    testcontainers::wait_for::Command cond;
    cond.cmd = {"true"};
    cond.poll_interval = std::chrono::milliseconds(50);
    testcontainers::detail::wait_until_ready(client, "abc", {WaitFor{cond}},
                                             std::chrono::seconds(10));

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 5u); // ping, create(409), create, start, inspect
    EXPECT_TRUE(request_is(requests[1], "POST /containers/abc/exec")) << requests[1];
    EXPECT_TRUE(request_is(requests[2], "POST /containers/abc/exec")) << requests[2];
}

TEST(CommandWait, NotFoundPropagates) {
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {},          // hijack, abandoned when the create fails
        {ping_ok()}, // session
        {http_response(404, "Not Found", R"({"message":"No such container: abc"})")},
    });
    DockerClient client = fast_client(server);

    EXPECT_THROW(testcontainers::detail::wait_until_ready(
                     client, "abc", {testcontainers::wait_for::successful_command({"true"})},
                     std::chrono::seconds(5)),
                 NotFoundError);

    EXPECT_EQ(server.requests().size(), 2u); // ping + the one create — no retry
}

TEST(CommandWait, EmptyCommandThrows) {
    CannedHttpServer server(std::vector<std::string>{});
    DockerClient client = fast_client(server);

    try {
        testcontainers::detail::wait_until_ready(
            client, "abc", {WaitFor{testcontainers::wait_for::Command{}}}, std::chrono::seconds(1));
        FAIL() << "expected DockerError";
    } catch (const DockerError& e) {
        EXPECT_TRUE(contains(e.what(), "non-empty command")) << e.what();
    }
    EXPECT_TRUE(server.requests().empty()); // rejected before any daemon interaction
}
