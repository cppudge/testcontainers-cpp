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

/// Deliver a streaming response body to `consumer` chunk by chunk. The caller
/// has already read the header into `parser` (a buffer_body parser).
/// read_some (not read): read returns only when the whole message is complete
/// — for a follow stream that means "when the container stops", batching all
/// output to the end; read_some returns after each socket read, so frames
/// arrive as the daemon flushes them. With `tty` the stream is raw/unframed
/// and is delivered verbatim as stdout; otherwise the multiplexed frames are
/// demuxed (stdin frames never appear in log/exec output and are skipped).
/// Returns when the stream ends, the daemon resets it, or `consumer` returns
/// false — the caller closes the connection either way.
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
        if (tty) {
            if (!consumer(LogSource::Stdout, std::string_view(buf.data(), n))) {
                stop = true;
            }
            continue;
        }
        for (const auto& frame : demuxer.feed(std::string_view(buf.data(), n))) {
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
                stop = true; // consumer asked to stop
                break;
            }
        }
    }
}

} // namespace testcontainers::docker
