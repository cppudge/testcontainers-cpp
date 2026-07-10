#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include "docker/Transport.hpp"

// The interleaved full-duplex pump behind the concrete transports'
// ITransport::exchange overrides. In its own header (not Transport.cpp) so the
// drain/re-arm mechanics are unit-testable over a mock stream — the hangs this
// code can produce are exactly the kind a live-socket test masks (see
// tests/unit/TransportExchangeTest.cpp's mock-stream cases).

namespace testcontainers::docker::detail {

/// The genuinely interleaved ITransport::exchange shared by the concrete
/// transports: one write chain pushes `input` while one read chain delivers
/// arriving chunks, both in flight at once on the transport's private
/// io_context, driven from the calling thread — so a peer that echoes a large
/// `input` back can never wedge the send side (the sequential base fallback
/// can). Outcome reporting is the ITransport::exchange contract.
///
/// `send_eof` half-closes the send side and runs from the OUTER loop only,
/// never from inside a completion handler: the named pipe's EOF needs its own
/// io_context round (run_pending / an overlapped zero-length write), and
/// io_context::restart() inside an active run() is undefined — which is also
/// why the pipe passes a pump-safe EOF initiator here instead of its
/// shutdown_send() (that one drives a NESTED run_pending and, with the read
/// chain re-arming forever, would not return until the io deadline drained).
///
/// The wait is bounded per progress event (either direction) by `io_timeout`
/// while the input is still going out — a wedged peer cannot hold the pump
/// beyond the idle deadline — and absolutely by `deadline` when given; once
/// the input has been sent, reads wait unbounded unless `deadline` says
/// otherwise (a long-running silent command is legitimate). Because progress
/// in EITHER direction re-arms the idle bound, a peer that floods output while
/// never draining the input holds the input phase open indefinitely — accepted
/// (the connection is demonstrably live; the deadline overloads bound it
/// absolutely), though the pre-pump sequential write would have timed out.
template <class Stream, class SendEof, class Cancel>
void duplex_exchange(boost::asio::io_context& ioc, Stream& stream,
                     const std::optional<std::chrono::milliseconds>& io_timeout,
                     std::string_view input, bool eof_after_input,
                     const ITransport::ChunkSink& on_chunk,
                     const std::optional<std::chrono::steady_clock::time_point>& deadline,
                     SendEof send_eof, Cancel cancel, boost::system::error_code& ec) {
    namespace asio = boost::asio;

    ec = {};
    std::array<char, 8192> buf{};
    bool read_done = false;            // the read chain decides the outcome
    boost::system::error_code read_ec; // stays {} on a consumer stop
    std::size_t sent = 0;
    bool writes_drained = input.empty(); // the async write chain has finished
    bool eof_owed = eof_after_input;
    boost::system::error_code send_ec; // recorded only: the read side reports
                                       // the real outcome (the peer may exit
                                       // without consuming all of its stdin)
    // Set before the final drain. cancel() only reaches operations still
    // pending in the OS — a SUCCESSFUL completion already queued on the
    // io_context is executed by the drain, and its handler must not re-arm
    // its chain then: the fresh operation would be one cancel() never saw,
    // and the unbounded drain run() below would block on it forever.
    bool stopping = false;

    std::function<void()> start_read = [&] {
        stream.async_read_some(asio::buffer(buf),
                               [&](const boost::system::error_code& op_ec, std::size_t n) {
                                   boost::system::error_code end_ec = op_ec;
                                   if (!end_ec && n == 0) {
                                       // A successful zero-byte read is the peer's half-close
                                       // (the named pipe's zero-length EOF message — see
                                       // read_some): map it to eof exactly like the sync path,
                                       // or this chain would spin re-reading it forever.
                                       end_ec = asio::error::eof;
                                   }
                                   if (stopping) {
                                       // Drained during the epilogue: never deliver or re-arm
                                       // (the outcome was already decided by the break).
                                       return;
                                   }
                                   if (n != 0 && !on_chunk(buf.data(), n)) {
                                       read_done = true; // consumer stop: a clean end (read_ec {})
                                       return;
                                   }
                                   if (end_ec) {
                                       read_ec = end_ec;
                                       read_done = true;
                                       return;
                                   }
                                   start_read();
                               });
    };
    // Bounded chunks, not one giant write: a multi-megabyte single overlapped
    // write would be accepted whole into kernel buffering (defeating the
    // input-phase idle guard below), page-lock the entire input at once, and
    // land on a message-mode pipe as ONE oversized message.
    constexpr std::size_t kWriteChunk = std::size_t{64} * 1024;
    std::function<void()> start_write = [&] {
        const std::size_t chunk = std::min(kWriteChunk, input.size() - sent);
        stream.async_write_some(asio::buffer(input.data() + sent, chunk),
                                [&](const boost::system::error_code& op_ec, std::size_t n) {
                                    sent += n;
                                    if (op_ec) {
                                        send_ec = op_ec;
                                        writes_drained = true;
                                        eof_owed = false; // no EOF down a broken send side
                                        return;
                                    }
                                    if (stopping) {
                                        writes_drained = true; // do not re-arm in the drain
                                        return;
                                    }
                                    if (sent < input.size()) {
                                        start_write();
                                        return;
                                    }
                                    writes_drained = true;
                                });
    };

    start_read();
    if (!writes_drained) {
        start_write();
    }

    bool expired = false;
    // read_done flips inside the chain handlers that ioc.run_one_for() below
    // executes — the analysis cannot see through the io_context.
    // NOLINTNEXTLINE(bugprone-infinite-loop)
    while (!read_done) {
        if (writes_drained && eof_owed) {
            send_eof(); // outer loop only (see above); a pipe flush may block
            eof_owed = false;
        }
        std::optional<std::chrono::milliseconds> wait;
        if (!writes_drained && io_timeout) {
            wait = *io_timeout; // per-progress idle bound for the input phase
        }
        if (deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                *deadline - std::chrono::steady_clock::now());
            if (left <= std::chrono::milliseconds::zero()) {
                expired = true;
                break;
            }
            wait = wait ? std::min(*wait, left) : left;
        }
        ioc.restart();
        const std::size_t ran = wait ? ioc.run_one_for(*wait) : ioc.run_one();
        if (ran == 0) {
            expired = true; // no progress within the budget
            break;
        }
    }

    // Cancel whatever is still in flight and DRAIN it: the pending handlers
    // reference this frame (buf / sent / the chain lambdas), so every one of
    // them must have completed before returning (run_pending's discipline).
    // `stopping` keeps the drained handlers from re-arming their chains (a
    // fresh operation would outlive the cancel and block the run() below).
    stopping = true;
    cancel();
    ioc.restart();
    ioc.run();

    // The outcome follows WHY the loop ended, never what the drain happened
    // to complete afterwards: an expiry stays timed_out even if the stream's
    // genuine end was sitting in the queue behind it.
    if (expired || !read_done || read_ec == asio::error::operation_aborted) {
        ec = asio::error::timed_out;
        return;
    }
    ec = read_ec; // {} for a consumer stop; the read chain's end error otherwise
}

} // namespace testcontainers::docker::detail
