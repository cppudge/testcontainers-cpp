#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/optional.hpp>

#include "docker/StreamRead.hpp"
#include "docker/Transport.hpp"
#include "testcontainers/docker/Logs.hpp"

// Tests in this file:
//   StreamRead.DemuxesFramesSplitAcrossReads - multiplexed stdout/stderr frames split at odd chunk boundaries are demuxed and routed to the consumer intact.
//   StreamRead.SkipsStdinFrames - a stdin-kind frame in the stream is dropped; surrounding stdout frames still arrive.
//   StreamRead.StopsWhenConsumerReturnsFalse - returning false from the consumer stops delivery without draining the remaining frames.
//   StreamRead.TtyPassthroughIsRawStdout - with tty=true the unframed body bytes are delivered verbatim as stdout (no demuxing).

using namespace testcontainers;
namespace http = boost::beast::http;

namespace {

/// An ITransport serving a canned byte stream, split into the given chunks so
/// each read_some returns at most one chunk's remainder — this is what forces
/// frame headers/payloads to arrive split across reads. EOF after the last
/// chunk (asio::error::eof, which Beast turns into a clean end of the
/// EOF-delimited response body).
class FakeTransport : public docker::ITransport {
public:
    explicit FakeTransport(std::vector<std::string> chunks) : chunks_(std::move(chunks)) {}

    std::size_t read_some(void* data, std::size_t size, boost::system::error_code& ec) override {
        if (index_ >= chunks_.size()) {
            ec = boost::asio::error::eof;
            return 0;
        }
        const std::string& cur = chunks_[index_];
        const std::size_t n = std::min(size, cur.size() - offset_);
        std::memcpy(data, cur.data() + offset_, n);
        offset_ += n;
        if (offset_ == cur.size()) {
            ++index_;
            offset_ = 0;
        }
        ec = {};
        return n;
    }
    std::size_t write_some(const void* /*data*/, std::size_t size,
                           boost::system::error_code& ec) override {
        ec = {};
        return size; // pretend everything was written
    }
    void shutdown_send() override {}
    bool supports_half_close() const noexcept override { return true; }
    void close() override {}

private:
    std::vector<std::string> chunks_;
    std::size_t index_ = 0;
    std::size_t offset_ = 0;
};

/// One multiplexed log frame: 8-byte header {kind, 0, 0, 0, len_be32} + payload.
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

constexpr const char* kHeader = "HTTP/1.1 200 OK\r\nServer: test\r\n\r\n";

/// Read the header off the fake stream, then run stream_body_to_consumer,
/// collecting (source, chunk) pairs until `keep_going` says stop.
std::vector<std::pair<LogSource, std::string>>
run_stream(std::vector<std::string> chunks, bool tty,
           const std::function<bool(std::size_t delivered)>& keep_going = {}) {
    FakeTransport transport(std::move(chunks));
    docker::TransportStream stream{transport};
    boost::beast::flat_buffer buffer;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(boost::none);

    boost::system::error_code ec;
    http::read_header(stream, buffer, parser, ec);
    EXPECT_FALSE(ec) << ec.message();

    std::vector<std::pair<LogSource, std::string>> out;
    docker::stream_body_to_consumer(
        stream, buffer, parser, tty, [&](LogSource source, std::string_view data) {
            out.emplace_back(source, std::string(data));
            return keep_going ? keep_going(out.size()) : true;
        });
    return out;
}

/// Concatenate the collected chunks of one source (chunk boundaries are an
/// implementation detail of the reads; only the byte sequence is contractual).
std::string joined(const std::vector<std::pair<LogSource, std::string>>& got, LogSource source) {
    std::string all;
    for (const auto& [src, data] : got) {
        if (src == source) {
            all += data;
        }
    }
    return all;
}

} // namespace

TEST(StreamRead, DemuxesFramesSplitAcrossReads) {
    const std::string body = frame(1, "hello from stdout") + frame(2, "err!");
    // Split mid-frame-header and mid-payload to exercise the demuxer's
    // incremental reassembly through the read loop.
    const std::string full = kHeader + body;
    std::vector<std::string> chunks;
    for (std::size_t i = 0; i < full.size(); i += 11) {
        chunks.push_back(full.substr(i, 11));
    }

    const auto got = run_stream(std::move(chunks), /*tty*/ false);
    EXPECT_EQ(joined(got, LogSource::Stdout), "hello from stdout");
    EXPECT_EQ(joined(got, LogSource::Stderr), "err!");
}

TEST(StreamRead, SkipsStdinFrames) {
    const std::string body = frame(1, "before") + frame(0, "stdin noise") + frame(1, "after");
    const auto got = run_stream({kHeader + body}, /*tty*/ false);

    EXPECT_EQ(joined(got, LogSource::Stdout), "beforeafter");
    EXPECT_EQ(joined(got, LogSource::Stderr), "");
}

TEST(StreamRead, StopsWhenConsumerReturnsFalse) {
    const std::string body = frame(1, "first") + frame(1, "second") + frame(1, "third");
    const auto got = run_stream({kHeader + body}, /*tty*/ false,
                                [](std::size_t delivered) { return delivered < 1; });

    ASSERT_EQ(got.size(), 1u); // stopped after the first delivery
    EXPECT_EQ(got[0].second, "first");
}

TEST(StreamRead, TtyPassthroughIsRawStdout) {
    // A TTY stream has no frame headers; the bytes must arrive verbatim, all
    // attributed to stdout, even when the reads split them arbitrarily.
    const std::string body = "raw tty bytes\r\nwith no framing";
    const std::string full = kHeader + body;
    std::vector<std::string> chunks;
    for (std::size_t i = 0; i < full.size(); i += 7) {
        chunks.push_back(full.substr(i, 7));
    }

    const auto got = run_stream(std::move(chunks), /*tty*/ true);
    EXPECT_EQ(joined(got, LogSource::Stdout), body);
    for (const auto& [source, data] : got) {
        EXPECT_EQ(source, LogSource::Stdout);
    }
}
