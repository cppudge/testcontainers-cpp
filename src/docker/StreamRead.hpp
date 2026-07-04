#pragma once

#include <array>
#include <cstddef>
#include <string_view>

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
/// demuxed. Returns when the stream ends, the daemon resets it, or `consumer`
/// returns false — the caller closes the connection either way.
inline void stream_body_to_consumer(
    TransportStream& stream, boost::beast::flat_buffer& buffer,
    boost::beast::http::response_parser<boost::beast::http::buffer_body>& parser, bool tty,
    const LogConsumer& consumer) {
    namespace http = boost::beast::http;

    LogDemuxer demuxer;
    std::array<char, 8192> buf{};
    boost::system::error_code ec;
    bool stop = false;
    while (!stop && !parser.is_done()) {
        parser.get().body().data = buf.data();
        parser.get().body().size = buf.size();

        http::read_some(stream, buffer, parser, ec);
        if (ec == http::error::need_buffer) {
            ec = {}; // the buffer filled up: not an error, just keep reading
        }
        if (ec) {
            break; // end_of_stream (the stream ended) or reset by the daemon
        }

        const std::size_t n = buf.size() - parser.get().body().size;
        if (n == 0) {
            continue;
        }
        stop = !detail::deliver_chunk(demuxer, std::string_view(buf.data(), n), tty, consumer);
    }
}

/// Read a hijacked (101-upgraded) stream to completion and return its raw
/// bytes: after the 101 the exec output arrives directly on the connection,
/// NOT as an HTTP body, so it is drained off the transport until EOF.
/// `leftover` is whatever the header parse already pulled past the 101 header.
/// A cleanly ended stream reports no error in `ec` — and the clean end DIFFERS
/// by transport: a socket ends with `eof`, but a peer-closed named pipe ends
/// with `broken_pipe` (asio maps only ERROR_HANDLE_EOF to eof; a pipe close is
/// ERROR_BROKEN_PIPE), so on the primary Windows transport `broken_pipe` IS
/// the normal completion, not a failure. Anything else is left in `ec` for the
/// caller to throw on.
inline std::string read_raw_stream(ITransport& transport, std::string_view leftover,
                                   boost::system::error_code& ec) {
    std::string out(leftover);
    std::array<char, 8192> buf{};
    for (;;) {
        const std::size_t n = transport.read_some(buf.data(), buf.size(), ec);
        out.append(buf.data(), n);
        if (ec) {
            break;
        }
    }
    if (ec == boost::asio::error::eof || ec == boost::asio::error::broken_pipe) {
        ec = {}; // the stream ending is the normal completion
    }
    return out;
}

/// Streaming sibling of read_raw_stream: deliver a hijacked (101-upgraded)
/// stream to `consumer` chunk by chunk (`leftover` first), demuxing unless
/// `tty`. Returns when the stream ends, the daemon resets it, or `consumer`
/// returns false — the caller closes the connection either way.
inline void stream_raw_to_consumer(ITransport& transport, std::string_view leftover, bool tty,
                                   const LogConsumer& consumer) {
    LogDemuxer demuxer;
    if (!leftover.empty() && !detail::deliver_chunk(demuxer, leftover, tty, consumer)) {
        return;
    }
    std::array<char, 8192> buf{};
    boost::system::error_code ec;
    for (;;) {
        const std::size_t n = transport.read_some(buf.data(), buf.size(), ec);
        if (n != 0 && !detail::deliver_chunk(demuxer, std::string_view(buf.data(), n), tty,
                                             consumer)) {
            return;
        }
        if (ec) {
            return; // stream ended or reset — the caller closes either way
        }
    }
}

} // namespace testcontainers::docker
