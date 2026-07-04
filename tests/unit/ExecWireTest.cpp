#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <chrono>

#include "CannedHttpServer.hpp"
#include "testcontainers/ExecOptions.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/Timeouts.hpp"

// Tests in this file (DockerClient::exec wire behavior against a canned
// loopback responder; no Docker daemon):
//   ExecWire.StdinRequestsConnectionUpgrade - exec with stdin_data sends "Connection: Upgrade" + "Upgrade: tcp" on the start request (replacing the default "Connection: close"), and the 200 fallback (a daemon that ignores the upgrade) still returns the demuxed output — stdin is fed after the header without breaking the body read.
//   ExecWire.NoStdinAlsoRequestsUpgrade - exec WITHOUT stdin sends the same upgrade headers (CLI parity; the non-upgraded exec-start response is never terminated by some daemons - observed on a Windows-containers 29.1.5), and a 200 from a daemon that ignores the upgrade still delivers the demuxed body.
//   ExecWire.UpgradedStreamReadsRaw - a 101 reply routes the output through the raw read path: frames arriving with (or after) the 101 header are demuxed, not parsed as an HTTP body.

namespace {

using tcunit::CannedHttpServer;
using tcunit::http_response;

using testcontainers::DockerClient;
using testcontainers::ExecOptions;
using testcontainers::ExecResult;

/// One multiplexed frame: 8-byte header {kind, 0, 0, 0, len_be32} + payload.
std::string frame(unsigned char kind, std::string_view payload) {
    std::string f;
    f.push_back(static_cast<char>(kind));
    f.append(3, '\0');
    const auto len = static_cast<std::uint32_t>(payload.size());
    f.push_back(static_cast<char>((len >> 24) & 0xFF));
    f.push_back(static_cast<char>((len >> 16) & 0xFF));
    f.push_back(static_cast<char>((len >> 8) & 0xFF));
    f.push_back(static_cast<char>(len & 0xFF));
    f.append(payload);
    return f;
}

std::string exec_created() { return http_response(201, "Created", R"({"Id":"e1"})"); }
std::string exec_inspected() { return http_response(200, "OK", R"({"ExitCode":0})"); }

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
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

// Connection order (== accept order): exec connects its hijack transport
// FIRST (its request arrives only after the create round-trip), so
// connection 0 is the exec-start stream, 1 is the create POST, 2 the
// inspect GET.

} // namespace

TEST(ExecWire, StdinRequestsConnectionUpgrade) {
    // The exec-start connection is answered with a PLAIN 200 (a daemon that
    // ignores the upgrade request); the empty second entry absorbs the stdin
    // bytes + EOF the client sends after the header.
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {http_response(200, "OK", frame(1, "out")), ""},
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

TEST(ExecWire, UpgradedStreamReadsRaw) {
    // The daemon honors the upgrade: 101, then the frames arrive RAW on the
    // connection (written here in one blob with the header, so part of the
    // stream lands in the client's header-parse buffer — the leftover path).
    const std::string upgraded =
        "HTTP/1.1 101 UPGRADED\r\n"
        "Content-Type: application/vnd.docker.raw-stream\r\n"
        "Connection: Upgrade\r\nUpgrade: tcp\r\n\r\n" +
        frame(1, "out") + frame(2, "err");
    CannedHttpServer server(std::vector<std::vector<std::string>>{
        {upgraded, ""}, // "": absorb the client's stdin + EOF, then close
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
