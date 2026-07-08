#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "TestSupport.hpp"
#include "docker/LogDemux.hpp"

// Tests in this file:
//   LogDemux.SingleStdoutFrame - a lone stdout frame decodes to one frame with the stdout kind and its payload.
//   LogDemux.SingleStderrFrame - a lone stderr frame decodes with the stderr kind.
//   LogDemux.MultipleInterleavedFrames - back-to-back stdout/stderr frames decode in order with correct kinds.
//   LogDemux.EmptyPayloadFrame - a zero-length-payload frame decodes without swallowing the following frame.
//   LogDemux.FrameSplitInsideHeader - a frame whose 8-byte header is split across two feeds decodes once complete.
//   LogDemux.FrameSplitInsidePayload - a frame whose payload is split across two feeds decodes once complete.
//   LogDemux.MultipleFramesSplitAtArbitraryPoints - five frames fed one byte at a time all decode correctly in order.
//   LogDemux.ByteByByteMatchesDemuxAll - feeding a multi-frame buffer one byte at a time yields the same frames as demux_all of the whole buffer.
//   LogDemux.ManySmallFramesInVaryingChunks - many small frames fed in deterministically cycling chunk sizes reassemble to the expected stdout/stderr.
//   LogDemux.DemuxAllConcatenatesByStream - demux_all concatenates stdout frames and stderr frames into their respective buffers.
//   LogDemux.DemuxAllIgnoresStdin - demux_all drops stdin frames and keeps only stdout/stderr.

using testcontainers::docker::demux_all;
using testcontainers::docker::LogDemuxer;
using testcontainers::docker::LogFrame;
using testcontainers::docker::LogStreamKind;

namespace {

// Encode one multiplexed log frame for the given stream. The kind-to-wire-byte
// mapping stays an explicit switch (the decoder under test must not share the
// encoder's assumptions); the byte packing itself is the shared tcunit::frame.
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
    return tcunit::frame(type, payload);
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

TEST(LogDemux, ByteByByteMatchesDemuxAll) {
    std::string buf;
    buf += encode_frame(LogStreamKind::StdOut, "first line\n");
    buf += encode_frame(LogStreamKind::StdErr, "an error\n");
    buf += encode_frame(LogStreamKind::StdOut, "second line\n");

    // Feeding one byte at a time exercises the partial-header / partial-payload
    // paths and the lazy compaction of the consumed offset.
    LogDemuxer demux;
    std::vector<LogFrame> all;
    for (char ch : buf) {
        for (auto& f : demux.feed(std::string(1, ch))) {
            all.push_back(std::move(f));
        }
    }

    // Must match the single-shot demux of the same bytes, frame for frame.
    const auto expected = demux_all(buf);
    std::string stdout_data;
    std::string stderr_data;
    for (const auto& f : all) {
        if (f.stream == LogStreamKind::StdOut) {
            stdout_data += f.data;
        } else if (f.stream == LogStreamKind::StdErr) {
            stderr_data += f.data;
        }
    }
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(stdout_data, expected.stdout_data);
    EXPECT_EQ(stderr_data, expected.stderr_data);
    EXPECT_EQ(all[0].stream, LogStreamKind::StdOut);
    EXPECT_EQ(all[0].data, "first line\n");
    EXPECT_EQ(all[1].stream, LogStreamKind::StdErr);
    EXPECT_EQ(all[1].data, "an error\n");
    EXPECT_EQ(all[2].stream, LogStreamKind::StdOut);
    EXPECT_EQ(all[2].data, "second line\n");
}

TEST(LogDemux, ManySmallFramesInVaryingChunks) {
    // Build N small frames, alternating stdout/stderr, with the index baked into
    // each payload so the expected concatenation is unambiguous.
    constexpr int kFrames = 200;
    std::string buf;
    std::string expected_stdout;
    std::string expected_stderr;
    for (int i = 0; i < kFrames; ++i) {
        const bool to_stdout = (i % 2 == 0);
        const std::string payload = (to_stdout ? "o" : "e") + std::to_string(i) + "\n";
        buf += encode_frame(to_stdout ? LogStreamKind::StdOut : LogStreamKind::StdErr, payload);
        (to_stdout ? expected_stdout : expected_stderr) += payload;
    }

    // Feed in deterministically varying chunk sizes (cycle 1, 3, 7 bytes) so the
    // split points fall arbitrarily across headers and payloads — no real RNG.
    static constexpr std::size_t kChunkSizes[] = {1, 3, 7};
    LogDemuxer demux;
    std::string stdout_data;
    std::string stderr_data;
    std::size_t off = 0;
    std::size_t which = 0;
    while (off < buf.size()) {
        const std::size_t n = std::min(kChunkSizes[which % 3], buf.size() - off);
        ++which;
        for (const auto& f : demux.feed(std::string_view(buf).substr(off, n))) {
            if (f.stream == LogStreamKind::StdOut) {
                stdout_data += f.data;
            } else if (f.stream == LogStreamKind::StdErr) {
                stderr_data += f.data;
            }
        }
        off += n;
    }

    EXPECT_EQ(stdout_data, expected_stdout);
    EXPECT_EQ(stderr_data, expected_stderr);
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
