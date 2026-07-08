#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/optional.hpp>

#include "TestSupport.hpp"
#include "docker/StreamRead.hpp"
#include "docker/Transport.hpp"
#include "testcontainers/docker/Logs.hpp"

// Tests in this file:
//   StreamRead.DemuxesFramesSplitAcrossReads - multiplexed stdout/stderr frames split at odd chunk boundaries are demuxed and routed to the consumer intact.
//   StreamRead.SkipsStdinFrames - a stdin-kind frame in the stream is dropped; surrounding stdout frames still arrive.
//   StreamRead.StopsWhenConsumerReturnsFalse - returning false from the consumer stops delivery without draining the remaining frames.
//   StreamRead.TtyPassthroughIsRawStdout - with tty=true the unframed body bytes are delivered verbatim as stdout (no demuxing).
//   StreamRead.RawStreamAccumulatesLeftoverAndReads - read_raw_stream (the 101-upgraded exec path) returns leftover + everything until EOF, with EOF reported as a clean end.
//   StreamRead.RawStreamPropagatesRealErrors - a mid-stream transport error (not eof/broken_pipe) is left in ec for the caller to throw on.
//   StreamRead.RawStreamTreatsBrokenPipeAsCleanEnd - a peer-closed NAMED PIPE ends the stream with broken_pipe (not eof) — that is the normal completion on the primary Windows transport, never an error.
//   StreamRead.RawConsumerDemuxesLeftoverFirst - stream_raw_to_consumer demuxes frames split between the header-parse leftover and the transport reads.
//   StreamRead.RawConsumerStopsWhenConsumerReturnsFalse - returning false stops the raw stream delivery early.
//   StreamRead.ReportsWhyDeliveryEnded - stream_body_to_consumer returns StreamEnded on EOF and ConsumerStopped on an early stop.
//   StreamRead.DeadlineArmsIoTimeoutPerRead - with a deadline, the transport's io deadline is re-armed with a positive remaining budget before every read.
//   StreamRead.TimedOutReadReportsDeadlineExpired - a read failing with asio timed_out (the re-armed io deadline firing) ends delivery with DeadlineExpired; chunks already delivered stay delivered.
//   StreamRead.ExpiredDeadlineEndsBeforeReading - a deadline already in the past reports DeadlineExpired without reading or delivering anything.

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
    void set_io_timeout(std::optional<std::chrono::milliseconds> timeout) override {
        io_timeouts_set.push_back(timeout);
    }
    void close() override {}

    /// Every set_io_timeout call, in order (the bounded delivery re-arms the
    /// io deadline before each read).
    std::vector<std::optional<std::chrono::milliseconds>> io_timeouts_set;

private:
    std::vector<std::string> chunks_;
    std::size_t index_ = 0;
    std::size_t offset_ = 0;
};

using tcunit::frame;

constexpr const char* kHeader = "HTTP/1.1 200 OK\r\nServer: test\r\n\r\n";

/// Read the header off `transport`, then run stream_body_to_consumer,
/// collecting (source, chunk) pairs until `keep_going` says stop. Returns why
/// the delivery ended (the collected chunks land in `out`).
FollowEnd run_stream_on(docker::ITransport& transport, bool tty,
                        std::vector<std::pair<LogSource, std::string>>& out,
                        const std::function<bool(std::size_t delivered)>& keep_going = {},
                        std::optional<std::chrono::steady_clock::time_point> deadline = {}) {
    docker::TransportStream stream{transport};
    boost::beast::flat_buffer buffer;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(boost::none);

    boost::system::error_code ec;
    http::read_header(stream, buffer, parser, ec);
    EXPECT_FALSE(ec) << ec.message();

    return docker::stream_body_to_consumer(
        transport, stream, buffer, parser, tty,
        [&](LogSource source, std::string_view data) {
            out.emplace_back(source, std::string(data));
            return keep_going ? keep_going(out.size()) : true;
        },
        deadline);
}

/// One-shot convenience over run_stream_on for the tests that only care about
/// the delivered chunks.
std::vector<std::pair<LogSource, std::string>>
run_stream(std::vector<std::string> chunks, bool tty,
           const std::function<bool(std::size_t delivered)>& keep_going = {}) {
    FakeTransport transport(std::move(chunks));
    std::vector<std::pair<LogSource, std::string>> out;
    run_stream_on(transport, tty, out, keep_going);
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

TEST(StreamRead, RawStreamAccumulatesLeftoverAndReads) {
    // The 101-upgrade exec path: the header parse pulled some stream bytes
    // into its buffer already (the leftover); the rest arrives from the
    // transport until EOF — which is the NORMAL completion, not an error.
    FakeTransport transport({"middle-", "end"});
    boost::system::error_code ec;
    const std::string out = docker::read_raw_stream(transport, "leftover-", ec);

    EXPECT_FALSE(ec) << ec.message();
    EXPECT_EQ(out, "leftover-middle-end");
}

namespace {

/// FakeTransport variant failing with the given error after its chunks.
class FailingTransport : public FakeTransport {
public:
    FailingTransport(std::vector<std::string> chunks, boost::system::error_code fail_with)
        : FakeTransport(std::move(chunks)), fail_with_(fail_with) {}

    std::size_t read_some(void* data, std::size_t size, boost::system::error_code& ec) override {
        const std::size_t n = FakeTransport::read_some(data, size, ec);
        if (ec == boost::asio::error::eof) {
            ec = fail_with_;
        }
        return n;
    }

private:
    boost::system::error_code fail_with_;
};

} // namespace

TEST(StreamRead, RawStreamPropagatesRealErrors) {
    FailingTransport transport({"partial"}, boost::asio::error::connection_reset);
    boost::system::error_code ec;
    const std::string out = docker::read_raw_stream(transport, "", ec);

    EXPECT_EQ(out, "partial"); // what arrived is still returned
    EXPECT_EQ(ec, boost::asio::error::connection_reset) << ec.message();
}

TEST(StreamRead, RawStreamTreatsBrokenPipeAsCleanEnd) {
    // On the primary Windows transport a peer-closed named pipe surfaces as
    // broken_pipe, NOT eof (asio maps only ERROR_HANDLE_EOF to eof) — so
    // broken_pipe is the NORMAL completion of a real named-pipe exec stream.
    // This pin keeps a "simplification" from dropping the broken_pipe clause
    // and breaking every real exec-with-stdin.
    FailingTransport transport({"all the output"}, boost::asio::error::broken_pipe);
    boost::system::error_code ec;
    const std::string out = docker::read_raw_stream(transport, "", ec);

    EXPECT_FALSE(ec) << ec.message();
    EXPECT_EQ(out, "all the output");
}

TEST(StreamRead, RawConsumerDemuxesLeftoverFirst) {
    // A frame split between the leftover (from the header parse) and the
    // transport reads must reassemble: the demuxer state carries across.
    const std::string body = frame(1, "out") + frame(2, "err");
    const std::string leftover = body.substr(0, 13); // mid-second-frame-header
    FakeTransport transport({body.substr(13)});

    std::vector<std::pair<LogSource, std::string>> got;
    docker::stream_raw_to_consumer(transport, leftover, /*tty*/ false,
                                   [&](LogSource source, std::string_view data) {
                                       got.emplace_back(source, std::string(data));
                                       return true;
                                   });

    EXPECT_EQ(joined(got, LogSource::Stdout), "out");
    EXPECT_EQ(joined(got, LogSource::Stderr), "err");
}

TEST(StreamRead, RawConsumerStopsWhenConsumerReturnsFalse) {
    const std::string body = frame(1, "first") + frame(1, "second");
    FakeTransport transport({body});

    std::vector<std::string> got;
    docker::stream_raw_to_consumer(transport, "", /*tty*/ false,
                                   [&](LogSource, std::string_view data) {
                                       got.emplace_back(data);
                                       return false; // stop after the first
                                   });

    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], "first");
}

TEST(StreamRead, ReportsWhyDeliveryEnded) {
    const std::string body = frame(1, "first") + frame(1, "second");
    {
        FakeTransport transport({kHeader + body});
        std::vector<std::pair<LogSource, std::string>> got;
        EXPECT_EQ(run_stream_on(transport, /*tty*/ false, got), FollowEnd::StreamEnded);
        EXPECT_EQ(got.size(), 2u);
    }
    {
        FakeTransport transport({kHeader + body});
        std::vector<std::pair<LogSource, std::string>> got;
        EXPECT_EQ(run_stream_on(transport, /*tty*/ false, got,
                                [](std::size_t delivered) { return delivered < 1; }),
                  FollowEnd::ConsumerStopped);
        EXPECT_EQ(got.size(), 1u);
    }
}

TEST(StreamRead, DeadlineArmsIoTimeoutPerRead) {
    const std::string body = frame(1, "one") + frame(2, "two");
    FakeTransport transport({kHeader, body.substr(0, 10), body.substr(10)});
    std::vector<std::pair<LogSource, std::string>> got;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    EXPECT_EQ(run_stream_on(transport, /*tty*/ false, got, {}, deadline), FollowEnd::StreamEnded);
    EXPECT_EQ(joined(got, LogSource::Stdout), "one");
    EXPECT_EQ(joined(got, LogSource::Stderr), "two");

    // Every read was preceded by a re-arm with the remaining (positive,
    // deadline-bounded) budget — this is what bounds a silent stream.
    ASSERT_FALSE(transport.io_timeouts_set.empty());
    for (const auto& timeout : transport.io_timeouts_set) {
        ASSERT_TRUE(timeout.has_value());
        EXPECT_GT(*timeout, std::chrono::milliseconds::zero());
        EXPECT_LE(*timeout, std::chrono::minutes(5));
    }
}

TEST(StreamRead, TimedOutReadReportsDeadlineExpired) {
    // The re-armed io deadline firing surfaces as asio timed_out from the
    // transport read; delivery must end with DeadlineExpired, keeping the
    // chunks that made it.
    FailingTransport transport({kHeader + frame(1, "partial")}, boost::asio::error::timed_out);
    std::vector<std::pair<LogSource, std::string>> got;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    EXPECT_EQ(run_stream_on(transport, /*tty*/ false, got, {}, deadline),
              FollowEnd::DeadlineExpired);
    EXPECT_EQ(joined(got, LogSource::Stdout), "partial");
}

TEST(StreamRead, ExpiredDeadlineEndsBeforeReading) {
    FakeTransport transport({kHeader + frame(1, "never delivered")});
    docker::TransportStream stream{transport};
    boost::beast::flat_buffer buffer;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(boost::none);
    boost::system::error_code ec;
    http::read_header(stream, buffer, parser, ec);
    ASSERT_FALSE(ec) << ec.message();

    const auto deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    bool delivered = false;
    const FollowEnd end = docker::stream_body_to_consumer(
        transport, stream, buffer, parser, /*tty*/ false,
        [&](LogSource, std::string_view) {
            delivered = true;
            return true;
        },
        deadline);

    EXPECT_EQ(end, FollowEnd::DeadlineExpired);
    EXPECT_FALSE(delivered);
    EXPECT_TRUE(transport.io_timeouts_set.empty()); // ended before any re-arm
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
