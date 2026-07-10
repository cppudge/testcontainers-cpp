#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <chrono>

#include "CannedHttpServer.hpp"
#include "TestSupport.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/ExecOptions.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/Logs.hpp"
#include "testcontainers/docker/Timeouts.hpp"

// Tests in this file (DockerClient::exec wire behavior against a canned
// loopback responder; no Docker daemon):
//   ExecWire.StdinRequestsConnectionUpgrade - exec with stdin_data sends "Connection: Upgrade" + "Upgrade: tcp" on the start request (replacing the default "Connection: close"), and the 200 fallback (a daemon that ignores the upgrade) still returns the demuxed output — stdin is fed after the header without breaking the body read.
//   ExecWire.NoStdinAlsoRequestsUpgrade - exec WITHOUT stdin sends the same upgrade headers (CLI parity; the non-upgraded exec-start response is never terminated by some daemons - observed on a Windows-containers 29.1.5), and a 200 from a daemon that ignores the upgrade still delivers the demuxed body.
//   ExecWire.DetachedStartIsPlainRequest - detach=true runs three plain request/response connections (ping, attach-nothing create, "Detach":true start WITHOUT upgrade headers), never inspects, and returns a default ExecResult.
//   ExecWire.DetachRejectsStdin - detach + stdin_data throws DockerError naming both options before any connection is opened (no abandoned exec instance).
//   ExecWire.DetachRejectsConsumer - detach on the streaming overload throws DockerError naming the consumer before any connection is opened.
//   ExecWire.UpgradedStreamReadsRaw - a 101 reply routes the output through the raw read path: frames arriving with (or after) the 101 header are demuxed, not parsed as an HTTP body.
//   ExecWire.DeadlineOverloadReportsExitCodeWhenFinished - the deadline overload delivers the stream, reports StreamEnded, and carries the exit code the inspect returned for the finished command.
//   ExecWire.DeadlineOverloadNoExitCodeWhileRunning - after an early consumer stop with the command still running, the inspect's null ExitCode reads as "no exit code" (never a parse error or a fake 0) and the result says ConsumerStopped.
//   ExecWire.StreamingConsumerStopReadsExitZeroWhileRunning - the plain streaming overload keeps its historical contract through the shared-impl rewire: a still-running command after an early stop reads as exit code 0.

namespace {

using tcunit::CannedHttpServer;
using tcunit::contains;
using tcunit::frame;
using tcunit::http_response;
using tcunit::ping_ok;

using testcontainers::DockerClient;
using testcontainers::DockerError;
using testcontainers::ExecOptions;
using testcontainers::ExecResult;
using testcontainers::LogSource;

std::string exec_created() { return tcunit::created("e1"); }
std::string exec_inspected() { return http_response(200, "OK", R"({"ExitCode":0})"); }

/// A client with a short io deadline so a regression fails in seconds, not
/// the 60s default.
DockerClient fast_client(const CannedHttpServer& server) {
    DockerClient client{server.host()};
    testcontainers::docker::TransportTimeouts timeouts;
    timeouts.io = std::chrono::seconds(2);
    client.set_transport_timeouts(timeouts);
    return client;
}

// Connection order (== accept order): exec connects its hijack transport
// FIRST (its request arrives only after the create round-trip), so
// connection 0 is the exec-start stream, 1 the version-negotiation ping
// (triggered by the exec-create call), 2 the create POST, 3 the inspect GET.

} // namespace

TEST(ExecWire, StdinRequestsConnectionUpgrade) {
    // The exec-start connection is answered with a PLAIN 200 (a daemon that
    // ignores the upgrade request); the empty second entry absorbs the stdin
    // bytes + EOF the client sends after the header.
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {http_response(200, "OK", frame(1, "out")), ""},
        {ping_ok()},
        {exec_created()},
        {exec_inspected()},
    });
    DockerClient client = fast_client(server);

    ExecOptions opts;
    opts.stdin_data = "ping\n";
    const ExecResult res = client.exec("abc", {"cat"}, opts);

    // The 200 fallback still delivers the demuxed body.
    EXPECT_EQ(res.stdout_data, "out");
    EXPECT_EQ(res.exit_code, 0);

    const auto by_connection = server.requests_by_connection();
    ASSERT_GE(by_connection.size(), 2u);
    ASSERT_FALSE(by_connection[0].empty());
    const std::string& start_request = by_connection[0][0];
    EXPECT_TRUE(contains(start_request, "POST /exec/e1/start")) << start_request;
    // The whole named-pipe-proxy fix hinges on these two headers: without the
    // upgrade, intermediaries drop client bytes sent after the POST.
    EXPECT_TRUE(contains(start_request, "Connection: Upgrade\r\n")) << start_request;
    EXPECT_TRUE(contains(start_request, "Upgrade: tcp\r\n")) << start_request;
    // set() REPLACED the default close — not appended alongside it.
    EXPECT_FALSE(contains(start_request, "Connection: close")) << start_request;
}

TEST(ExecWire, NoStdinAlsoRequestsUpgrade) {
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {http_response(200, "OK", frame(1, "out"))},
        {ping_ok()},
        {exec_created()},
        {exec_inspected()},
    });
    DockerClient client = fast_client(server);

    const ExecResult res = client.exec("abc", {"echo", "out"});

    // A daemon that ignores the upgrade answers 200 — the HTTP-body path
    // still delivers the demuxed output.
    EXPECT_EQ(res.stdout_data, "out");
    const auto by_connection = server.requests_by_connection();
    ASSERT_FALSE(by_connection.empty());
    ASSERT_FALSE(by_connection[0].empty());
    const std::string& start_request = by_connection[0][0];
    // Every exec start hijacks like the docker CLI does, stdin or not: some
    // daemons never terminate a NON-upgraded exec-start response (observed on
    // a Windows-containers 29.1.5 — the body read hung waiting for an EOF
    // that never came), while the upgraded stream is closed on exec exit.
    EXPECT_TRUE(contains(start_request, "Connection: Upgrade\r\n")) << start_request;
    EXPECT_TRUE(contains(start_request, "Upgrade: tcp\r\n")) << start_request;
}

TEST(ExecWire, DetachedStartIsPlainRequest) {
    // A detached exec never hijacks a connection: ping, create, start are
    // three ordinary one-shot exchanges (accept order = connection order here,
    // unlike the attached tests above — no early hijack connect), and there is
    // NO exec-inspect afterwards (the command is still running, so there is no
    // exit code to read).
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {ping_ok()},
        {exec_created()},
        {http_response(200, "OK", "")},
    });
    DockerClient client = fast_client(server);

    ExecOptions opts;
    opts.detach = true;
    const ExecResult res = client.exec("abc", {"sleep", "30"}, opts);

    // Fire-and-forget: the result keeps its defaults.
    EXPECT_TRUE(res.stdout_data.empty());
    EXPECT_TRUE(res.stderr_data.empty());
    EXPECT_EQ(res.exit_code, 0);

    const auto by_connection = server.requests_by_connection();
    ASSERT_EQ(by_connection.size(), 3u); // ping, create, start — no inspect
    ASSERT_FALSE(by_connection[1].empty());
    // The create body attaches nothing: a detached exec streams nothing back.
    const std::string& create_request = by_connection[1][0];
    EXPECT_TRUE(contains(create_request, "POST /containers/abc/exec")) << create_request;
    EXPECT_FALSE(contains(create_request, "AttachStdout")) << create_request;
    EXPECT_FALSE(contains(create_request, "AttachStderr")) << create_request;
    ASSERT_FALSE(by_connection[2].empty());
    const std::string& start_request = by_connection[2][0];
    EXPECT_TRUE(contains(start_request, "POST /exec/e1/start")) << start_request;
    EXPECT_TRUE(contains(start_request, R"("Detach":true)")) << start_request;
    // No upgrade: a detached start is an ordinary request/response round-trip
    // (`docker exec -d` parity), so the default close header survives.
    EXPECT_FALSE(contains(start_request, "Connection: Upgrade")) << start_request;
    EXPECT_TRUE(contains(start_request, "Connection: close")) << start_request;
}

TEST(ExecWire, DetachRejectsStdin) {
    // The invalid combination is rejected BEFORE any daemon interaction: no
    // connection is opened and no exec instance is left behind on the daemon.
    CannedHttpServer server(std::vector<std::vector<std::string>>{{ping_ok()}});
    DockerClient client = fast_client(server);

    ExecOptions opts;
    opts.detach = true;
    opts.stdin_data = "ping\n";
    try {
        (void)client.exec("abc", {"cat"}, opts);
        FAIL() << "detach + stdin_data must throw DockerError";
    } catch (const DockerError& e) {
        EXPECT_TRUE(contains(e.what(), "detach")) << e.what();
        EXPECT_TRUE(contains(e.what(), "stdin_data")) << e.what();
    }
    EXPECT_TRUE(server.requests_by_connection().empty());
}

TEST(ExecWire, DetachRejectsConsumer) {
    // Same up-front rejection for the streaming overload: a detached exec
    // attaches no streams, so no output could ever reach the consumer.
    CannedHttpServer server(std::vector<std::vector<std::string>>{{ping_ok()}});
    DockerClient client = fast_client(server);

    ExecOptions opts;
    opts.detach = true;
    try {
        (void)client.exec("abc", {"sleep", "30"}, opts,
                          [](LogSource, std::string_view) { return true; });
        FAIL() << "detach + consumer must throw DockerError";
    } catch (const DockerError& e) {
        EXPECT_TRUE(contains(e.what(), "detach")) << e.what();
        EXPECT_TRUE(contains(e.what(), "consumer")) << e.what();
    }
    EXPECT_TRUE(server.requests_by_connection().empty());
}

TEST(ExecWire, UpgradedStreamReadsRaw) {
    // The daemon honors the upgrade: 101, then the frames arrive RAW on the
    // connection (written here in one blob with the header, so part of the
    // stream lands in the client's header-parse buffer — the leftover path).
    const std::string upgraded = "HTTP/1.1 101 UPGRADED\r\n"
                                 "Content-Type: application/vnd.docker.raw-stream\r\n"
                                 "Connection: Upgrade\r\nUpgrade: tcp\r\n\r\n" +
                                 frame(1, "out") + frame(2, "err");
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {upgraded, ""}, // "": absorb the client's stdin + EOF, then close
        {ping_ok()},
        {exec_created()},
        {exec_inspected()},
    });
    DockerClient client = fast_client(server);

    ExecOptions opts;
    opts.stdin_data = "ping\n";
    const ExecResult res = client.exec("abc", {"cat"}, opts);

    EXPECT_EQ(res.stdout_data, "out");
    EXPECT_EQ(res.stderr_data, "err");
    EXPECT_EQ(res.exit_code, 0);
}

namespace {

/// A 101-upgraded exec-start reply carrying `body` as the raw stream.
std::string upgraded_with(const std::string& body) {
    return "HTTP/1.1 101 UPGRADED\r\n"
           "Content-Type: application/vnd.docker.raw-stream\r\n"
           "Connection: Upgrade\r\nUpgrade: tcp\r\n\r\n" +
           body;
}

} // namespace

TEST(ExecWire, DeadlineOverloadReportsExitCodeWhenFinished) {
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {upgraded_with(frame(1, "done"))},
        {ping_ok()},
        {exec_created()},
        {http_response(200, "OK", R"({"Running":false,"ExitCode":3})")},
    });
    DockerClient client = fast_client(server);

    std::string collected;
    const testcontainers::ExecStreamResult res = client.exec(
        "abc", {"sh", "-c", "exit 3"}, ExecOptions{},
        [&](LogSource, std::string_view data) {
            collected.append(data);
            return true;
        },
        std::chrono::steady_clock::now() + std::chrono::minutes(5));

    EXPECT_EQ(res.end, testcontainers::FollowEnd::StreamEnded);
    ASSERT_TRUE(res.exit_code.has_value());
    EXPECT_EQ(*res.exit_code, 3);
    EXPECT_EQ(collected, "done");
}

TEST(ExecWire, DeadlineOverloadNoExitCodeWhileRunning) {
    // The consumer stops early and the exec inspect reports the command still
    // running. ExitCode is null then (moby serializes a pointer type) — that
    // must read as "no exit code yet", never as a parse error or a fake 0.
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {upgraded_with(frame(1, "first") + frame(1, "second"))},
        {ping_ok()},
        {exec_created()},
        {http_response(200, "OK", R"({"Running":true,"ExitCode":null})")},
    });
    DockerClient client = fast_client(server);

    const testcontainers::ExecStreamResult res = client.exec(
        "abc", {"sh"}, ExecOptions{}, [](LogSource, std::string_view) { return false; },
        std::chrono::steady_clock::now() + std::chrono::minutes(5));

    EXPECT_EQ(res.end, testcontainers::FollowEnd::ConsumerStopped);
    EXPECT_FALSE(res.exit_code.has_value());
}

TEST(ExecWire, StreamingConsumerStopReadsExitZeroWhileRunning) {
    // The plain (no-deadline) streaming overload keeps its historical contract
    // through the shared-impl rewire: a command still running after an early
    // consumer stop reads as exit code 0 (and the null ExitCode never throws).
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {upgraded_with(frame(1, "first") + frame(1, "second"))},
        {ping_ok()},
        {exec_created()},
        {http_response(200, "OK", R"({"Running":true,"ExitCode":null})")},
    });
    DockerClient client = fast_client(server);

    const ExecResult res = client.exec("abc", {"sh"}, ExecOptions{},
                                       [](LogSource, std::string_view) { return false; });

    EXPECT_EQ(res.exit_code, 0);
    EXPECT_TRUE(res.stdout_data.empty());
}
