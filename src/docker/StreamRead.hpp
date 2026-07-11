#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>

#include <boost/asio/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>

#include "docker/LogDemux.hpp"
#include "docker/Transport.hpp"
#include "testcontainers/docker/Logs.hpp"

// Incremental delivery of a streaming Docker response body (follow-logs / exec
// attach) to a LogConsumer. Separated from DockerClient so the chunk-routing
// logic (need_buffer recovery, early stop, tty vs demuxed) is unit-testable
// over a fake ITransport without a daemon (see tests/unit/StreamReadTest.cpp).

namespace testcontainers::docker {

namespace detail {

/// Route one decoded chunk to `consumer`: verbatim as stdout with `tty`,
/// otherwise demuxed through `demuxer` (stdin frames never appear in log/exec
/// output and are skipped). Returns false when the consumer asked to stop.
inline bool deliver_chunk(LogDemuxer& demuxer, std::string_view chunk, bool tty,
                          const LogConsumer& consumer) {
    if (tty) {
        return consumer(LogSource::Stdout, chunk);
    }
    for (const auto& frame : demuxer.feed(chunk)) {
        LogSource source = LogSource::Stdout;
        switch (frame.stream) {
        case LogStreamKind::StdIn:
            continue;
        case LogStreamKind::StdOut:
            source = LogSource::Stdout;
            break;
        case LogStreamKind::StdErr:
            source = LogSource::Stderr;
            break;
        }
        if (!consumer(source, frame.data)) {
            return false;
        }
    }
    return true;
}

} // namespace detail

/// Deliver a streaming response body to `consumer` chunk by chunk. The caller
/// has already read the header into `parser` (a buffer_body parser).
/// read_some (not read): read returns only when the whole message is complete
/// — for a follow stream that means "when the container stops", batching all
/// output to the end; read_some returns after each socket read, so frames
/// arrive as the daemon flushes them. With `tty` the stream is raw/unframed
/// and is delivered verbatim as stdout; otherwise the multiplexed frames are
/// demuxed.
///
/// With a `deadline`, `transport`'s io deadline is re-armed with the remaining
/// budget before every read, so the whole delivery — chunks trickling or none
/// arriving at all — is bounded by the deadline; without one, the transport's
/// current io deadline (the caller typically disables it for follow streams)
/// stays in effect. Returns why delivery ended; the caller closes the
/// connection in every case.
inline FollowEnd stream_body_to_consumer(
    ITransport& transport, TransportStream& stream, boost::beast::flat_buffer& buffer,
    boost::beast::http::response_parser<boost::beast::http::buffer_body>& parser, bool tty,
    const LogConsumer& consumer,
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) {
    namespace http = boost::beast::http;

    LogDemuxer demuxer;
    std::array<char, 8192> buf{};
    boost::system::error_code ec;
    while (!parser.is_done()) {
        if (deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                *deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero()) {
                return FollowEnd::DeadlineExpired;
            }
            transport.set_io_timeout(remaining);
        }

        parser.get().body().data = buf.data();
        parser.get().body().size = buf.size();

        http::read_some(stream, buffer, parser, ec);
        if (ec == http::error::need_buffer) {
            ec = {}; // the buffer filled up: not an error, just keep reading
        }
        if (ec == boost::asio::error::timed_out) {
            return FollowEnd::DeadlineExpired; // the re-armed io deadline fired
        }
        if (ec) {
            // end_of_stream (the stream ended) or reset by the daemon.
            return FollowEnd::StreamEnded;
        }

        const std::size_t n = buf.size() - parser.get().body().size;
        if (n == 0) {
            continue;
        }
        if (!detail::deliver_chunk(demuxer, std::string_view(buf.data(), n), tty, consumer)) {
            return FollowEnd::ConsumerStopped;
        }
    }
    return FollowEnd::StreamEnded;
}

/// Exec-attach pump for a hijacked (101-upgraded) stream WITH stdin: hand the
/// transport the stdin bytes to write (half-closing after them) INTERLEAVED
/// with the output read — see ITransport::exchange — delivering each arriving
/// chunk to `consumer` (`leftover`, already in memory, first), demuxing
/// unless `tty`. A command that echoes a large stdin back can no longer
/// backpressure the write into a timeout. End reporting mirrors
/// stream_raw_to_consumer: read errors (the stream ending, a reset) are
/// StreamEnded, an expired `deadline` is DeadlineExpired (nothing delivered
/// when it was already past at entry). `wedge_ec` is set — and the FollowEnd
/// meaningless — ONLY when the exchange timed out with NO deadline given:
/// the input-phase idle guard tripped (a peer consuming neither direction),
/// which the caller reports as its transport error.
inline FollowEnd pump_exec_stream(ITransport& transport, std::string_view leftover,
                                  std::string_view stdin_data, bool tty,
                                  const LogConsumer& consumer,
                                  std::optional<std::chrono::steady_clock::time_point> deadline,
                                  boost::system::error_code& wedge_ec) {
    wedge_ec = {};
    LogDemuxer demuxer;
    if (deadline && *deadline <= std::chrono::steady_clock::now()) {
        return FollowEnd::DeadlineExpired;
    }
    if (!leftover.empty() && !detail::deliver_chunk(demuxer, leftover, tty, consumer)) {
        return FollowEnd::ConsumerStopped;
    }
    bool stopped = false;
    boost::system::error_code ec;
    transport.exchange(
        stdin_data, /*eof_after_input=*/true,
        [&](const char* data, std::size_t size) {
            if (!detail::deliver_chunk(demuxer, std::string_view(data, size), tty, consumer)) {
                stopped = true;
                return false;
            }
            return true;
        },
        deadline, ec);
    if (stopped) {
        return FollowEnd::ConsumerStopped;
    }
    if (ec == boost::asio::error::timed_out) {
        if (deadline) {
            return FollowEnd::DeadlineExpired;
        }
        wedge_ec = ec; // the input-phase idle guard: the caller throws
        return FollowEnd::StreamEnded;
    }
    return FollowEnd::StreamEnded; // eof / broken_pipe / reset: the stream is over
}

/// Deliver a hijacked (101-upgraded) stream to `consumer` chunk by chunk
/// (`leftover` — whatever the header parse already pulled past the 101 header
/// — first), demuxing unless `tty`: after the 101 the output arrives raw on
/// the connection, NOT as an HTTP body. Any read-side end maps to StreamEnded
/// — a socket ends with `eof`, a peer-closed named pipe with `broken_pipe`
/// (asio maps only ERROR_HANDLE_EOF to eof), and dockerd RESETS a hijacked
/// connection still holding unconsumed stdin — all of them are the peer
/// finishing; the caller closes the connection in every case.
///
/// With a `deadline`, the transport's io deadline is re-armed with the
/// remaining budget before every read (mirroring stream_body_to_consumer), so
/// a silent stream is bounded and ends with DeadlineExpired; a deadline that
/// has already passed ends delivery before anything is delivered — the
/// leftover included.
inline FollowEnd stream_raw_to_consumer(
    ITransport& transport, std::string_view leftover, bool tty, const LogConsumer& consumer,
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) {
    LogDemuxer demuxer;
    if (deadline && *deadline <= std::chrono::steady_clock::now()) {
        return FollowEnd::DeadlineExpired;
    }
    if (!leftover.empty() && !detail::deliver_chunk(demuxer, leftover, tty, consumer)) {
        return FollowEnd::ConsumerStopped;
    }
    std::array<char, 8192> buf{};
    boost::system::error_code ec;
    for (;;) {
        if (deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                *deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero()) {
                return FollowEnd::DeadlineExpired;
            }
            transport.set_io_timeout(remaining);
        }
        const std::size_t n = transport.read_some(buf.data(), buf.size(), ec);
        if (n != 0 &&
            !detail::deliver_chunk(demuxer, std::string_view(buf.data(), n), tty, consumer)) {
            return FollowEnd::ConsumerStopped;
        }
        if (ec == boost::asio::error::timed_out && deadline) {
            return FollowEnd::DeadlineExpired; // the re-armed io deadline fired
        }
        if (ec) {
            return FollowEnd::StreamEnded; // ended or reset — the caller closes either way
        }
    }
}

} // namespace testcontainers::docker
