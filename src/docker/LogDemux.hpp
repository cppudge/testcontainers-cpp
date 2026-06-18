#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Parser for the Docker multiplexed log stream. For a container started WITHOUT
// a TTY, `GET /containers/{id}/logs` returns a sequence of frames, each prefixed
// by an 8-byte header:
//
//   byte 0     : stream type — 0=stdin, 1=stdout, 2=stderr
//   bytes 1..3 : zero padding
//   bytes 4..7 : big-endian uint32 = payload length N
//   bytes 8..  : N payload bytes
//
// Kept separate from DockerClient so it can be unit-tested without a daemon or
// any networking (and with no Boost dependency).
namespace testcontainers::docker {

/// Which standard stream a frame belongs to.
enum class LogStreamKind { StdIn, StdOut, StdErr };

/// A single decoded log frame.
struct LogFrame {
    LogStreamKind stream = LogStreamKind::StdOut;
    std::string data;
};

/// Incremental, stateful demuxer for the multiplexed log stream. Feed it
/// arbitrary byte chunks; it buffers partial headers/payloads across calls and
/// emits frames as they complete. This makes split-buffer streaming correct.
class LogDemuxer {
public:
    /// Feed bytes; returns the frames completed by this chunk (may be empty).
    std::vector<LogFrame> feed(std::string_view bytes);

private:
    static constexpr std::size_t kHeaderSize = 8;

    std::string pending_;            ///< bytes not yet consumed into a frame
    bool have_header_ = false;       ///< true once the current frame's header is parsed
    LogStreamKind stream_ = LogStreamKind::StdOut; ///< current frame's stream
    std::uint32_t payload_len_ = 0;  ///< current frame's expected payload length
};

/// Combined stdout / stderr text from a complete log buffer.
struct DemuxedLogs {
    std::string stdout_data;
    std::string stderr_data;
};

/// Convenience for a complete buffer: demux into combined stdout / stderr.
/// Frames addressed to stdin are ignored (they never appear in log output).
DemuxedLogs demux_all(std::string_view body);

} // namespace testcontainers::docker
