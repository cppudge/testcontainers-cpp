#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "CannedHttpServer.hpp"
#include "TestSupport.hpp"
#include "docker/Auth.hpp"
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
//   CopyWire.SinkStreamsArchiveDownload - the copy_from_container sink overload GETs the archive and delivers the exact tar bytes in blocks.
//   CopyWire.DownloadErrorThrowsBeforeFirstBlock - a 404 on the streaming download throws NotFoundError and the sink never runs.
//   CopyWire.CopyFromToDirectoryExtracts - copy_from_container_to streams the archive straight into files under the destination directory.
//   CopyWire.StatParsesHeader - container_path_stat HEADs the archive endpoint and decodes the base64 X-Docker-Container-Path-Stat header (name/size/mode/dir bit).
//   CopyWire.StatParsesDirectoryBit - Go's ModeDir bit (1<<31) in the stat mode decodes as is_dir.
//   CopyWire.StatUndecodableHeaderThrows - a garbage X-Docker-Container-Path-Stat header surfaces as DockerError.
//   CopyWire.StatMissingPathThrowsNotFound - a 404 HEAD reply surfaces as NotFoundError.
//   CopyWire.CopyFromToDirectorySingleFile - a single-file archive lands as dest_dir/<basename>.

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

namespace {

/// An archive-download response: a tar body served with the right headers.
std::string archive_response(const std::string& tar) {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/x-tar\r\nContent-Length: " +
           std::to_string(tar.size()) + "\r\n\r\n" + tar;
}

/// A self-cleaning extraction destination.
class DestDir {
public:
    DestDir() {
        static std::atomic<unsigned> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("tc_copywire_dest_" + std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    ~DestDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    const std::filesystem::path& path() const { return dir_; }

private:
    std::filesystem::path dir_;
};

/// Read `path` fully (binary).
std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST(CopyWire, SinkStreamsArchiveDownload) {
    const std::string tar = testcontainers::docker::build_context_tar(
        {testcontainers::docker::TarFile{"x.txt", "downloaded-bytes"}});
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        archive_response(tar),
    });
    DockerClient client{server.host()};

    std::string received;
    std::size_t calls = 0;
    client.copy_from_container("abc123", "/tmp/x.txt", [&](const char* data, std::size_t size) {
        ++calls;
        received.append(data, size);
    });

    EXPECT_GE(calls, 1u);
    EXPECT_EQ(received, tar); // the raw tar bytes, byte-exact
    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_TRUE(tcunit::request_is(requests[1], "GET /containers/abc123/archive?path="))
        << requests[1];
}

TEST(CopyWire, DownloadErrorThrowsBeforeFirstBlock) {
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        tcunit::http_response(404, "Not Found", R"({"message":"no such path"})"),
    });
    DockerClient client{server.host()};

    bool sink_ran = false;
    try {
        client.copy_from_container("abc123", "/missing",
                                   [&](const char*, std::size_t) { sink_ran = true; });
        FAIL() << "copy_from_container did not throw on 404";
    } catch (const NotFoundError& e) {
        EXPECT_TRUE(tcunit::contains(e.what(), "copy_from_container(abc123")) << e.what();
    }
    EXPECT_FALSE(sink_ran);
}

TEST(CopyWire, CopyFromToDirectoryExtracts) {
    const std::string tar = testcontainers::docker::build_context_tar({
        testcontainers::docker::TarFile{"data/a.txt", "alpha"},
        testcontainers::docker::TarFile{"data/sub/b.txt", "beta"},
    });
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        archive_response(tar),
    });
    DockerClient client{server.host()};

    const DestDir dest;
    client.copy_from_container_to("abc123", "/data", dest.path());

    EXPECT_EQ(slurp(dest.path() / "data" / "a.txt"), "alpha");
    EXPECT_EQ(slurp(dest.path() / "data" / "sub" / "b.txt"), "beta");
}

TEST(CopyWire, StatParsesHeader) {
    const std::string stat_json =
        R"({"name":"x.txt","size":16,"mode":420,"mtime":"2026-07-10T10:00:00Z","linkTarget":""})";
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        "HTTP/1.1 200 OK\r\nX-Docker-Container-Path-Stat: " +
            testcontainers::docker::base64_encode(stat_json) +
            "\r\nContent-Length: 1536\r\n\r\n", // a HEAD reply: length advertised, no body
    });
    DockerClient client{server.host()};

    const DockerClient::ContainerPathStat stat = client.container_path_stat("abc123", "/tmp/x.txt");

    EXPECT_EQ(stat.name, "x.txt");
    EXPECT_EQ(stat.size, 16u);
    EXPECT_EQ(stat.mode, 420u); // 0644 in Go FileMode permission bits
    EXPECT_FALSE(stat.is_dir);
    EXPECT_EQ(stat.mtime, "2026-07-10T10:00:00Z");
    EXPECT_EQ(stat.link_target, "");

    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_TRUE(tcunit::request_is(requests[1], "HEAD /containers/abc123/archive?path="))
        << requests[1];
}

TEST(CopyWire, StatParsesDirectoryBit) {
    // Go's os.FileMode: bit 31 (2147483648) is ModeDir; 493 = 0755.
    const std::string stat_json =
        R"({"name":"tmp","size":4096,"mode":2147484141,"mtime":"2026-07-10T10:00:00Z"})";
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        "HTTP/1.1 200 OK\r\nX-Docker-Container-Path-Stat: " +
            testcontainers::docker::base64_encode(stat_json) + "\r\nContent-Length: 0\r\n\r\n",
    });
    DockerClient client{server.host()};

    const DockerClient::ContainerPathStat stat = client.container_path_stat("abc123", "/tmp");
    EXPECT_EQ(stat.name, "tmp");
    EXPECT_TRUE(stat.is_dir);
    EXPECT_EQ(stat.mode & 0777u, 0755u);
}

TEST(CopyWire, StatUndecodableHeaderThrows) {
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        "HTTP/1.1 200 OK\r\nX-Docker-Container-Path-Stat: !!!not-base64-json!!!\r\n"
        "Content-Length: 0\r\n\r\n",
    });
    DockerClient client{server.host()};

    try {
        client.container_path_stat("abc123", "/tmp/x");
        FAIL() << "container_path_stat did not throw on a garbage stat header";
    } catch (const DockerError& e) {
        EXPECT_TRUE(tcunit::contains(e.what(), "container_path_stat(abc123")) << e.what();
    }
}

TEST(CopyWire, StatMissingPathThrowsNotFound) {
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        tcunit::http_response(404, "Not Found", R"({"message":"no such path"})"),
    });
    DockerClient client{server.host()};

    EXPECT_THROW(client.container_path_stat("abc123", "/missing"), testcontainers::NotFoundError);
}

TEST(CopyWire, CopyFromToDirectorySingleFile) {
    // Docker archives a single-file path as one entry named by its base name.
    const std::string tar = testcontainers::docker::build_context_tar(
        {testcontainers::docker::TarFile{"note.txt", "single-file-body"}});
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        archive_response(tar),
    });
    DockerClient client{server.host()};

    const DestDir dest;
    client.copy_from_container_to("abc123", "/tmp/note.txt", dest.path());

    EXPECT_EQ(slurp(dest.path() / "note.txt"), "single-file-body");
}
