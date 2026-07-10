#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "CannedHttpServer.hpp"
#include "TestSupport.hpp"
#include "docker/Tar.hpp"
#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerClient.hpp"

// Tests in this file:
//   CopyWire.SingleSourceStreamsChunkedTar - copy_to_container PUTs the archive with Transfer-Encoding: chunked (no Content-Length), and the decoded body is a tar holding the source entry.
//   CopyWire.BatchedSourcesShareOnePut - the vector overload sends ONE PUT whose tar carries every source's entry in order.
//   CopyWire.EmptyBatchSkipsRequest - an empty source vector performs no request at all (not even the version ping).
//   CopyWire.ErrorStatusThrowsTyped - a 404 reply to the upload surfaces as NotFoundError carrying the copy context.
//   CopyWire.DaemonDroppingUploadSurfacesTypedError - a daemon that drops the connection mid-upload (no response at all) surfaces as a typed DockerError naming the call, never a hang.
//   CopyWire.DaemonEarlyErrorMidUploadSurfacesError - a daemon that 404s after the head and closes without draining surfaces as a DockerError (NotFoundError when the response outruns the reset).

using testcontainers::CopyToContainer;
using testcontainers::DockerClient;
using testcontainers::DockerError;
using testcontainers::NotFoundError;
using testcontainers::docker::extract_tar;
using testcontainers::docker::TarEntry;

namespace {

/// The decoded request body (everything after the head separator).
std::string body_of(const std::string& request) {
    const std::size_t head_end = request.find("\r\n\r\n");
    EXPECT_NE(head_end, std::string::npos);
    return head_end == std::string::npos ? std::string{} : request.substr(head_end + 4);
}

} // namespace

TEST(CopyWire, SingleSourceStreamsChunkedTar) {
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),                    // version negotiation
        tcunit::http_response(200, "OK", ""), // the upload's reply
    });
    DockerClient client{server.host()};

    client.copy_to_container("abc123", CopyToContainer::content("payload", "/tmp/x.txt"));

    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_TRUE(tcunit::request_is(requests[0], "GET /_ping"));
    const std::string& put = requests[1];
    EXPECT_TRUE(tcunit::request_is(put, "PUT /containers/abc123/archive?path=/")) << put;
    // Streamed upload: chunked framing, no precomputed length.
    EXPECT_TRUE(tcunit::contains(put, "Transfer-Encoding: chunked")) << put;
    EXPECT_FALSE(tcunit::contains(put, "Content-Length")) << put;
    EXPECT_TRUE(tcunit::contains(put, "Content-Type: application/x-tar")) << put;

    // The server records the body DECODED: it must be the tar itself.
    const std::vector<TarEntry> entries = extract_tar(body_of(put));
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "tmp/x.txt");
    EXPECT_EQ(entries[0].body, "payload");
}

TEST(CopyWire, BatchedSourcesShareOnePut) {
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        tcunit::http_response(200, "OK", ""),
    });
    DockerClient client{server.host()};

    const std::vector<CopyToContainer> sources = {
        CopyToContainer::content("one", "/a/first.txt"),
        CopyToContainer::content("two", "/b/second.txt").with_mode(0600),
    };
    client.copy_to_container("abc123", sources);

    // ONE upload round-trip regardless of the source count.
    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    const std::string& put = requests[1];
    EXPECT_TRUE(tcunit::request_is(put, "PUT /containers/abc123/archive?path=/")) << put;

    const std::vector<TarEntry> entries = extract_tar(body_of(put));
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "a/first.txt");
    EXPECT_EQ(entries[0].body, "one");
    EXPECT_EQ(entries[1].name, "b/second.txt");
    EXPECT_EQ(entries[1].body, "two");
    EXPECT_EQ(entries[1].mode & 0777, 0600);
}

TEST(CopyWire, EmptyBatchSkipsRequest) {
    tcunit::CannedHttpServer server(std::vector<std::string>{}); // no exchange expected
    DockerClient client{server.host()};

    client.copy_to_container("abc123", std::vector<CopyToContainer>{});

    EXPECT_TRUE(server.requests().empty());
}

TEST(CopyWire, DaemonDroppingUploadSurfacesTypedError) {
    // The "daemon" closes right after our request head without draining the
    // body. Depending on how much the OS buffered before the reset, the
    // failure surfaces as a broken body write, a killed response read, or an
    // EOF — every path must arrive as a typed DockerError naming the call
    // (and must not hang).
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        tcunit::respond_after_head(""), // drop the connection, no response
    });
    DockerClient client{server.host()};

    const std::string big(std::size_t{4} * 1024 * 1024, 'x'); // outruns socket buffers
    try {
        client.copy_to_container("abc123", CopyToContainer::content(big, "/tmp/big.bin"));
        FAIL() << "copy_to_container did not throw on a dropped upload";
    } catch (const DockerError& e) {
        EXPECT_TRUE(tcunit::contains(e.what(), "copy_to_container(abc123")) << e.what();
    }
}

TEST(CopyWire, DaemonEarlyErrorMidUploadSurfacesError) {
    // Same drop, but the daemon sends its 404 before closing. When the
    // response outruns the reset the typed NotFoundError comes through
    // (throw_upload_error's early-response read); when the reset kills it,
    // the transport error does — both are DockerError.
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        tcunit::respond_after_head(
            tcunit::http_response(404, "Not Found", R"({"message":"No such container"})")),
    });
    DockerClient client{server.host()};

    const std::string big(std::size_t{4} * 1024 * 1024, 'x');
    try {
        client.copy_to_container("abc123", CopyToContainer::content(big, "/tmp/big.bin"));
        FAIL() << "copy_to_container did not throw on a rejected upload";
    } catch (const DockerError& e) {
        EXPECT_TRUE(tcunit::contains(e.what(), "copy_to_container(abc123")) << e.what();
    }
}

TEST(CopyWire, ErrorStatusThrowsTyped) {
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        tcunit::http_response(404, "Not Found", R"({"message":"No such container: abc123"})"),
    });
    DockerClient client{server.host()};

    try {
        client.copy_to_container("abc123", CopyToContainer::content("x", "/tmp/x"));
        FAIL() << "copy_to_container did not throw on a 404 upload reply";
    } catch (const NotFoundError& e) {
        EXPECT_TRUE(tcunit::contains(e.what(), "copy_to_container(abc123")) << e.what();
        EXPECT_TRUE(tcunit::contains(e.what(), "No such container")) << e.what();
    }
}
