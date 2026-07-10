#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <vector>

#include "CannedHttpServer.hpp"
#include "TestSupport.hpp"
#include "docker/Tar.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/BuildOptions.hpp"
#include "testcontainers/docker/DockerClient.hpp"

// Tests in this file:
//   BuildWire.ProducerStreamsChunkedContext - the streaming build_image overload POSTs /build with Transfer-Encoding: chunked (tag in the query) and the decoded body is the produced context tar, including a lazy host-file descriptor read at upload time.
//   BuildWire.StringOverloadSendsIdenticalBody - the string overload rides the same chunked path and delivers the context bytes verbatim.
//   BuildWire.ErrorInStreamThrows - an {"error"} line embedded in the HTTP-200 build stream surfaces as DockerError from the streaming overload.

using testcontainers::DockerClient;
using testcontainers::DockerError;
using testcontainers::docker::build_context_tar;
using testcontainers::docker::BuildOptions;
using testcontainers::docker::extract_tar;
using testcontainers::docker::stream_context_tar;
using testcontainers::docker::TarEntry;
using testcontainers::docker::TarFile;

namespace {

/// The decoded request body (everything after the head separator).
std::string body_of(const std::string& request) {
    const std::size_t head_end = request.find("\r\n\r\n");
    EXPECT_NE(head_end, std::string::npos);
    return head_end == std::string::npos ? std::string{} : request.substr(head_end + 4);
}

BuildOptions tagged(const std::string& tag) {
    BuildOptions options;
    options.tag = tag;
    return options;
}

/// A successful one-step build stream (newline-delimited JSON, HTTP 200).
std::string ok_build_stream() {
    return tcunit::http_response(200, "OK", "{\"stream\":\"Step 1/1 : FROM scratch\\n\"}\n");
}

/// A self-cleaning temp file holding `content`.
class TempFile {
public:
    explicit TempFile(const std::string& content) {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("tc_buildwire_" + std::to_string(counter.fetch_add(1)));
        std::ofstream out(path_, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

TEST(BuildWire, ProducerStreamsChunkedContext) {
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        ok_build_stream(),
    });
    DockerClient client{server.host()};

    // One inline entry plus one LAZY host-file descriptor: the file's bytes
    // must be read while the upload streams, through the whole HTTP stack.
    const TempFile app_file("lazy-app-bytes");
    TarFile lazy;
    lazy.name = "app/data.bin";
    lazy.path = app_file.path();
    const std::vector<TarFile> files = {TarFile{"Dockerfile", "FROM scratch\n"}, lazy};

    client.build_image(
        [&files](const testcontainers::docker::ByteSink& sink) { stream_context_tar(files, sink); },
        tagged("app:v1"));

    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    const std::string& post = requests[1];
    EXPECT_TRUE(tcunit::request_is(post, "POST /build")) << post;
    EXPECT_TRUE(tcunit::contains(post, "t=app")) << post; // the tag rides the query
    EXPECT_TRUE(tcunit::contains(post, "Transfer-Encoding: chunked")) << post;
    EXPECT_FALSE(tcunit::contains(post, "Content-Length")) << post;
    EXPECT_TRUE(tcunit::contains(post, "Content-Type: application/x-tar")) << post;

    const std::vector<TarEntry> entries = extract_tar(body_of(post));
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "Dockerfile");
    EXPECT_EQ(entries[0].body, "FROM scratch\n");
    EXPECT_EQ(entries[1].name, "app/data.bin");
    EXPECT_EQ(entries[1].body, "lazy-app-bytes");
}

TEST(BuildWire, StringOverloadSendsIdenticalBody) {
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        ok_build_stream(),
    });
    DockerClient client{server.host()};

    const std::string context = build_context_tar({TarFile{"Dockerfile", "FROM scratch\n"}});
    client.build_image(context, tagged("app:v1"));

    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    // The wrapper sends the pre-built tar byte-for-byte (one chunk).
    EXPECT_EQ(body_of(requests[1]), context);
}

TEST(BuildWire, ErrorInStreamThrows) {
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::ping_ok(),
        tcunit::http_response(200, "OK",
                              "{\"stream\":\"Step 1/1 : RUN false\\n\"}\n"
                              "{\"errorDetail\":{\"message\":\"boom went the step\"},"
                              "\"error\":\"boom went the step\"}\n"),
    });
    DockerClient client{server.host()};

    const std::vector<TarFile> files = {TarFile{"Dockerfile", "FROM scratch\nRUN false\n"}};
    try {
        client.build_image(
            [&files](const testcontainers::docker::ByteSink& sink) {
                stream_context_tar(files, sink);
            },
            tagged("app:v1"));
        FAIL() << "build_image did not throw on an in-stream build error";
    } catch (const DockerError& e) {
        EXPECT_TRUE(tcunit::contains(e.what(), "boom went the step")) << e.what();
    }
}
