#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "docker/LogDemux.hpp"

// Tests in this file:
//   LogDemux.SingleStdoutFrame - a lone stdout frame decodes to one frame with the stdout kind and its payload.
//   LogDemux.SingleStderrFrame - a lone stderr frame decodes with the stderr kind.
//   LogDemux.MultipleInterleavedFrames - back-to-back stdout/stderr frames decode in order with correct kinds.
//   LogDemux.EmptyPayloadFrame - a zero-length-payload frame decodes without swallowing the following frame.
//   LogDemux.FrameSplitInsideHeader - a frame whose 8-byte header is split across two feeds decodes once complete.
//   LogDemux.FrameSplitInsidePayload - a frame whose payload is split across two feeds decodes once complete.
//   LogDemux.MultipleFramesSplitAtArbitraryPoints - five frames fed one byte at a time all decode correctly in order.
//   LogDemux.DemuxAllConcatenatesByStream - demux_all concatenates stdout frames and stderr frames into their respective buffers.
//   LogDemux.DemuxAllIgnoresStdin - demux_all drops stdin frames and keeps only stdout/stderr.

using testcontainers::docker::demux_all;
using testcontainers::docker::LogDemuxer;
using testcontainers::docker::LogFrame;
using testcontainers::docker::LogStreamKind;

namespace {

// Encode one multiplexed log frame: type byte + 3 zero pads + 4-byte big-endian
// length + payload. Mirrors Docker's wire format for non-TTY containers.
std::string encode_frame(LogStreamKind stream, std::string_view payload) {
    unsigned char type = 1; // stdout
    switch (stream) {
    case LogStreamKind::StdIn:
        type = 0;
        break;
    case LogStreamKind::StdOut:
        type = 1;
        break;
    case LogStreamKind::StdErr:
        type = 2;
        break;
    }
    const auto n = static_cast<std::uint32_t>(payload.size());
    std::string frame;
    frame.push_back(static_cast<char>(type));
    frame.push_back('\0');
    frame.push_back('\0');
    frame.push_back('\0');
    frame.push_back(static_cast<char>((n >> 24) & 0xFF));
    frame.push_back(static_cast<char>((n >> 16) & 0xFF));
    frame.push_back(static_cast<char>((n >> 8) & 0xFF));
    frame.push_back(static_cast<char>(n & 0xFF));
    frame.append(payload);
    return frame;
}

} // namespace

TEST(LogDemux, SingleStdoutFrame) {
    LogDemuxer demux;
    const auto frames = demux.feed(encode_frame(LogStreamKind::StdOut, "hello"));
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].stream, LogStreamKind::StdOut);
    EXPECT_EQ(frames[0].data, "hello");
}

TEST(LogDemux, SingleStderrFrame) {
    LogDemuxer demux;
    const auto frames = demux.feed(encode_frame(LogStreamKind::StdErr, "oops"));
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].stream, LogStreamKind::StdErr);
    EXPECT_EQ(frames[0].data, "oops");
}

TEST(LogDemux, MultipleInterleavedFrames) {
    std::string buf;
    buf += encode_frame(LogStreamKind::StdOut, "out1");
    buf += encode_frame(LogStreamKind::StdErr, "err1");
    buf += encode_frame(LogStreamKind::StdOut, "out2");

    LogDemuxer demux;
    const auto frames = demux.feed(buf);
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames[0].stream, LogStreamKind::StdOut);
    EXPECT_EQ(frames[0].data, "out1");
    EXPECT_EQ(frames[1].stream, LogStreamKind::StdErr);
    EXPECT_EQ(frames[1].data, "err1");
    EXPECT_EQ(frames[2].stream, LogStreamKind::StdOut);
    EXPECT_EQ(frames[2].data, "out2");
}

TEST(LogDemux, EmptyPayloadFrame) {
    std::string buf;
    buf += encode_frame(LogStreamKind::StdOut, "");
    buf += encode_frame(LogStreamKind::StdOut, "after");

    LogDemuxer demux;
    const auto frames = demux.feed(buf);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0].stream, LogStreamKind::StdOut);
    EXPECT_TRUE(frames[0].data.empty());
    EXPECT_EQ(frames[1].data, "after");
}

TEST(LogDemux, FrameSplitInsideHeader) {
    const std::string frame = encode_frame(LogStreamKind::StdOut, "payload");
    // Split at byte 3 — inside the 8-byte header.
    LogDemuxer demux;
    auto frames = demux.feed(std::string_view(frame).substr(0, 3));
    EXPECT_TRUE(frames.empty()); // header incomplete, nothing yet

    frames = demux.feed(std::string_view(frame).substr(3));
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].stream, LogStreamKind::StdOut);
    EXPECT_EQ(frames[0].data, "payload");
}

TEST(LogDemux, FrameSplitInsidePayload) {
    const std::string frame = encode_frame(LogStreamKind::StdErr, "abcdefgh");
    // Split at byte 11 — header (8) is complete but payload is only partial.
    LogDemuxer demux;
    auto frames = demux.feed(std::string_view(frame).substr(0, 11));
    EXPECT_TRUE(frames.empty()); // payload incomplete

    frames = demux.feed(std::string_view(frame).substr(11));
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].stream, LogStreamKind::StdErr);
    EXPECT_EQ(frames[0].data, "abcdefgh");
}

TEST(LogDemux, MultipleFramesSplitAtArbitraryPoints) {
    std::string buf;
    buf += encode_frame(LogStreamKind::StdOut, "alpha");
    buf += encode_frame(LogStreamKind::StdErr, "beta");
    buf += encode_frame(LogStreamKind::StdOut, "gamma");
    buf += encode_frame(LogStreamKind::StdErr, "");
    buf += encode_frame(LogStreamKind::StdOut, "delta");

    // Feed the buffer one byte at a time; collect every emitted frame.
    LogDemuxer demux;
    std::vector<LogFrame> all;
    for (char ch : buf) {
        const std::string one(1, ch);
        for (auto& f : demux.feed(one)) {
            all.push_back(std::move(f));
        }
    }

    ASSERT_EQ(all.size(), 5u);
    EXPECT_EQ(all[0].stream, LogStreamKind::StdOut);
    EXPECT_EQ(all[0].data, "alpha");
    EXPECT_EQ(all[1].stream, LogStreamKind::StdErr);
    EXPECT_EQ(all[1].data, "beta");
    EXPECT_EQ(all[2].stream, LogStreamKind::StdOut);
    EXPECT_EQ(all[2].data, "gamma");
    EXPECT_EQ(all[3].stream, LogStreamKind::StdErr);
    EXPECT_TRUE(all[3].data.empty());
    EXPECT_EQ(all[4].stream, LogStreamKind::StdOut);
    EXPECT_EQ(all[4].data, "delta");
}

TEST(LogDemux, DemuxAllConcatenatesByStream) {
    std::string buf;
    buf += encode_frame(LogStreamKind::StdOut, "out-a\n");
    buf += encode_frame(LogStreamKind::StdErr, "err-a\n");
    buf += encode_frame(LogStreamKind::StdOut, "out-b\n");
    buf += encode_frame(LogStreamKind::StdErr, "err-b\n");

    const auto logs = demux_all(buf);
    EXPECT_EQ(logs.stdout_data, "out-a\nout-b\n");
    EXPECT_EQ(logs.stderr_data, "err-a\nerr-b\n");
}

TEST(LogDemux, DemuxAllIgnoresStdin) {
    std::string buf;
    buf += encode_frame(LogStreamKind::StdIn, "should-be-dropped");
    buf += encode_frame(LogStreamKind::StdOut, "kept");

    const auto logs = demux_all(buf);
    EXPECT_EQ(logs.stdout_data, "kept");
    EXPECT_TRUE(logs.stderr_data.empty());
}
