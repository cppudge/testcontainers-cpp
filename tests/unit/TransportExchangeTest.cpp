#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include "LoopbackServer.hpp"
#include "docker/DuplexExchange.hpp"
#include "docker/Transport.hpp"
#include "testcontainers/docker/Timeouts.hpp"

// Tests in this file (loopback servers + a scripted mock stream, no Docker
// daemon — the interleaved full-duplex ITransport::exchange pump that
// exec-with-stdin rides):
//   TransportExchange.InterleavesLargeInputWithLargeOutput - a peer that writes its whole (multi-megabyte) output BEFORE reading any input can only be survived by interleaving: both blobs round-trip intact where a sequential write-then-read would wedge into the io deadline.
//   TransportExchange.EofDeliveredAfterInput - the peer's read-to-EOF loop ends (half-close delivered after the input), and output sent after that still arrives: the write chain, EOF, and read chain sequence correctly.
//   TransportExchange.ConsumerStopEndsExchangeCleanly - on_chunk returning false ends the exchange with a clean ec while a large write is still in flight (the pending write is cancelled and drained; no hang).
//   TransportExchange.DeadlineBoundsThePump - a peer that accepts and then ignores both directions is left behind when the absolute deadline passes: ec is timed_out, promptly.
//   TransportExchange.InputPhaseIdleGuardTrips - with no deadline, a peer consuming NEITHER direction trips the io-deadline idle guard while input is still pending: ec is timed_out instead of blocking forever (a generous absolute deadline is the loud safety net for kernels whose buffering would otherwise swallow the input whole and hang the test).
//   TransportExchange.PeerCloseWithInputPendingEndsExchange - a peer that closes immediately (a command exiting without reading its stdin) ends the exchange promptly with the read side's end error, never timed_out and never a hang.
//   TransportExchange.DrainDoesNotReArmAfterConsumerStop - (mock stream) a write completion already QUEUED when the consumer stops must not re-arm the write chain during the drain: no fresh operation is issued after the break (a re-armed one would outlive the cancel and hang the drain).
//   TransportExchange.ExpiredBreakStaysTimedOut - (mock stream) an expiry decides the outcome even when the stream's genuine end was already queued behind it: the drained EOF must not overwrite timed_out.

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;
using namespace std::chrono_literals;

using testcontainers::docker::ITransport;
using testcontainers::docker::TransportTimeouts;

// The shared one-connection loopback fixture; the receive_buffer option
// exists for this suite (shrink the peer's window so a writer runs out of
// in-flight room).
using LoopbackPeer = tcunit::LoopbackServer;

/// Drain `socket` until EOF/reset, returning everything read.
std::string read_to_end(tcp::socket& socket) {
    std::string all;
    char buf[65536];
    boost::system::error_code ec;
    while (!ec) {
        const std::size_t n = socket.read_some(asio::buffer(buf), ec);
        all.append(buf, n);
    }
    return all;
}

/// A recognizable filler that never repeats with the read-chunk sizes.
std::string patterned(std::size_t size, char base) {
    std::string data(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>(base + static_cast<char>(i % 23));
    }
    return data;
}

std::unique_ptr<ITransport> connect_with_io(const LoopbackPeer& peer,
                                            std::chrono::milliseconds io) {
    TransportTimeouts timeouts;
    timeouts.io = io;
    return testcontainers::docker::connect(peer.host(), timeouts);
}

} // namespace

TEST(TransportExchange, InterleavesLargeInputWithLargeOutput) {
    // The peer WRITES ITS WHOLE OUTPUT FIRST and only then reads the input —
    // the worst-case shape of a command that echoes/produces a lot while its
    // stdin is still arriving. 16 MiB each way is far beyond any socket
    // buffering, so a sequential write-then-read client wedges (its write
    // backpressures once the peer's send direction fills every buffer); only
    // the interleaved pump completes. The 5s io deadline turns a regression
    // into a fast failure instead of a hang.
    const std::string output = patterned(std::size_t{16} * 1024 * 1024, 'A');
    const std::string input = patterned(std::size_t{16} * 1024 * 1024, 'a');

    // Peer-side state is read only AFTER the peer's destructor joined its
    // thread — that join is the synchronization point.
    std::string peer_got;
    boost::system::error_code peer_write_ec;
    std::string got;
    boost::system::error_code ec;
    {
        LoopbackPeer peer([&](tcp::socket& socket) {
            asio::write(socket, asio::buffer(output), peer_write_ec);
            peer_got = read_to_end(socket); // until the client's half-close (EOF)
            boost::system::error_code ignore;
            socket.shutdown(tcp::socket::shutdown_both, ignore);
            socket.close(ignore);
        });
        const auto transport = connect_with_io(peer, 5s);

        transport->exchange(
            input, /*eof_after_input=*/true,
            [&](const char* data, std::size_t size) {
                got.append(data, size);
                return true;
            },
            std::nullopt, ec);
        transport->close();
    }

    EXPECT_FALSE(peer_write_ec) << peer_write_ec.message();
    EXPECT_TRUE(ec == asio::error::eof || ec == asio::error::broken_pipe ||
                ec == asio::error::connection_reset)
        << ec.message();
    ASSERT_EQ(got.size(), output.size());
    EXPECT_TRUE(got == output); // EQ would dump 16 MiB into the failure message
    ASSERT_EQ(peer_got.size(), input.size());
    EXPECT_TRUE(peer_got == input);
}

TEST(TransportExchange, EofDeliveredAfterInput) {
    // The peer consumes the input to EOF FIRST, then answers: the exchange
    // must have written everything, half-closed (or the peer's read loop
    // never ends), and still be reading when the answer arrives.
    std::string peer_got;
    std::string got;
    boost::system::error_code ec;
    {
        LoopbackPeer peer([&](tcp::socket& socket) {
            peer_got = read_to_end(socket);
            boost::system::error_code write_ec;
            asio::write(socket, asio::buffer(std::string("after-eof")), write_ec);
            boost::system::error_code ignore;
            socket.shutdown(tcp::socket::shutdown_both, ignore);
            socket.close(ignore);
        });
        const auto transport = connect_with_io(peer, 5s);

        transport->exchange(
            "hello, peer", /*eof_after_input=*/true,
            [&](const char* data, std::size_t size) {
                got.append(data, size);
                return true;
            },
            std::nullopt, ec);
        transport->close();
    }

    EXPECT_TRUE(ec == asio::error::eof || ec == asio::error::broken_pipe ||
                ec == asio::error::connection_reset)
        << ec.message();
    EXPECT_EQ(got, "after-eof");
    EXPECT_EQ(peer_got, "hello, peer");
}

TEST(TransportExchange, ConsumerStopEndsExchangeCleanly) {
    // The consumer declines after the first chunk while a large write is
    // still in flight: the exchange must end CLEANLY (empty ec) right away —
    // the pending write is cancelled and drained, not waited for.
    int chunks = 0;
    boost::system::error_code ec;
    {
        LoopbackPeer peer([&](tcp::socket& socket) {
            boost::system::error_code write_ec;
            asio::write(socket, asio::buffer(std::string("first-chunk")), write_ec);
            // Read whatever arrives until the client hangs up (tolerates a
            // reset from the cancelled client write).
            read_to_end(socket);
            boost::system::error_code ignore;
            socket.close(ignore);
        });
        const auto transport = connect_with_io(peer, 5s);

        const std::string input = patterned(std::size_t{64} * 1024 * 1024, 'a'); // still in flight
        transport->exchange(
            input, /*eof_after_input=*/true,
            [&](const char*, std::size_t) {
                ++chunks;
                return false; // stop on the first chunk
            },
            std::nullopt, ec);
        transport->close();
    }

    EXPECT_FALSE(ec) << ec.message();
    EXPECT_EQ(chunks, 1);
}

TEST(TransportExchange, DeadlineBoundsThePump) {
    LoopbackPeer peer; // accepts, then ignores both directions
    const auto transport = connect_with_io(peer, std::chrono::minutes(1));

    boost::system::error_code ec;
    const auto start = std::chrono::steady_clock::now();
    transport->exchange(
        "ping", /*eof_after_input=*/true, [](const char*, std::size_t) { return true; },
        start + 300ms, ec);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    transport->close();

    EXPECT_EQ(ec, asio::error::timed_out) << ec.message();
    EXPECT_LT(elapsed, 5s); // promptly, not the io deadline and not forever
}

TEST(TransportExchange, InputPhaseIdleGuardTrips) {
    // A peer consuming NEITHER direction while most of the input is still
    // pending: the io deadline is the idle guard — the pump must fail with
    // timed_out in io-deadline time instead of blocking (the pre-pump
    // sequential write had the same guarantee per write). The peer's receive
    // buffer is pinched to a few KB: loopback kernels otherwise absorb many
    // megabytes of "sent" input, and input the kernel has swallowed is out of
    // the guard's scope by design (the read side then waits unbounded, as a
    // long-running command is legitimate).
    LoopbackPeer peer({}, /*receive_buffer=*/4096); // accepts, then ignores both directions
    const auto transport = connect_with_io(peer, 300ms);

    const std::string input =
        patterned(std::size_t{8} * 1024 * 1024, 'a'); // beyond any in-flight room
    boost::system::error_code ec;
    const auto start = std::chrono::steady_clock::now();
    // The generous absolute deadline is a SAFETY NET, not the subject: if a
    // platform's loopback ignored the receive-buffer pinch and swallowed the
    // whole input, the pump would be in its by-design unbounded read wait and
    // the test would hang without it. The idle guard must fire long before
    // (the elapsed bound is what tells the two apart).
    transport->exchange(
        input, /*eof_after_input=*/true, [](const char*, std::size_t) { return true; }, start + 20s,
        ec);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    transport->close();

    EXPECT_EQ(ec, asio::error::timed_out) << ec.message();
    EXPECT_LT(elapsed, 10s);
}

TEST(TransportExchange, PeerCloseWithInputPendingEndsExchange) {
    // The peer closes without reading anything — the shape of a command that
    // exits instantly while its stdin is still going out. The exchange must
    // end promptly with the read side's end error (eof / reset / broken
    // pipe — platform-dependent), never a hang and never timed_out.
    LoopbackPeer peer([&](tcp::socket& socket) {
        boost::system::error_code ignore;
        socket.close(ignore);
    });
    const auto transport = connect_with_io(peer, 5s);

    const std::string input = patterned(std::size_t{64} * 1024 * 1024, 'a');
    boost::system::error_code ec;
    transport->exchange(
        input, /*eof_after_input=*/true, [](const char*, std::size_t) { return true; },
        std::nullopt, ec);
    transport->close();

    EXPECT_TRUE(ec == asio::error::eof || ec == asio::error::broken_pipe ||
                ec == asio::error::connection_reset || ec == asio::error::connection_aborted)
        << ec.message();
}

namespace {

/// A scriptable stream for driving detail::duplex_exchange directly: each
/// async op either POSTS its completion to the io_context up front — the
/// "already queued when the pump breaks" shape a live socket can only produce
/// by luck — or is held forever (the handler is discarded; no real operation
/// exists, so nothing fires later).
class MockStream {
public:
    explicit MockStream(asio::io_context& ioc) : ioc_(&ioc) {}

    struct ReadStep {
        std::string data;             ///< bytes to deliver (may be empty)
        boost::system::error_code ec; ///< completion code (eof for an end)
    };

    template <class MutableBuffers, class Handler>
    void async_read_some(const MutableBuffers& buffers, Handler handler) {
        ++reads_issued;
        if (read_script.empty()) {
            return; // held forever
        }
        ReadStep step = std::move(read_script.front());
        read_script.erase(read_script.begin());
        const std::size_t n =
            asio::buffer_copy(buffers, asio::buffer(step.data.data(), step.data.size()));
        asio::post(*ioc_,
                   [handler = std::move(handler), ec = step.ec, n]() mutable { handler(ec, n); });
    }

    template <class ConstBuffers, class Handler>
    void async_write_some(const ConstBuffers& buffers, Handler handler) {
        ++writes_issued;
        if (write_completions_left == 0) {
            return; // held forever
        }
        --write_completions_left;
        const std::size_t n = asio::buffer_size(buffers);
        asio::post(*ioc_, [handler = std::move(handler), n]() mutable {
            handler(boost::system::error_code{}, n);
        });
    }

    std::vector<ReadStep> read_script;
    int write_completions_left = 0;
    int reads_issued = 0;
    int writes_issued = 0;

private:
    asio::io_context* ioc_;
};

} // namespace

TEST(TransportExchange, DrainDoesNotReArmAfterConsumerStop) {
    // Script: the read completion (one chunk; the consumer stops on it) is
    // queued FIRST, the first write chunk's success completion SECOND. The
    // pump's loop runs ONE handler per iteration, so the consumer stop breaks
    // the loop with the write success still queued; the drain then executes
    // it — and must NOT re-arm the write chain (a fresh operation would
    // outlive the cancel and block the unbounded drain run() forever on a
    // real socket; here it is merely counted).
    asio::io_context ioc;
    MockStream stream(ioc);
    stream.read_script.push_back({"first-chunk", {}});
    stream.write_completions_left = 1;

    const std::string input = patterned(std::size_t{128} * 1024, 'a'); // two 64 KiB chunks
    boost::system::error_code ec;
    int chunks = 0;
    testcontainers::docker::detail::duplex_exchange(
        ioc, stream, std::chrono::milliseconds(5000), input, /*eof_after_input=*/true,
        [&](const char*, std::size_t) {
            ++chunks;
            return false; // stop on the first chunk
        },
        std::nullopt, [] {}, [] {}, ec);

    EXPECT_FALSE(ec) << ec.message();
    EXPECT_EQ(chunks, 1);
    // One read, one write — the drained write success issued no successor.
    EXPECT_EQ(stream.reads_issued, 1);
    EXPECT_EQ(stream.writes_issued, 1);
}

TEST(TransportExchange, ExpiredBreakStaysTimedOut) {
    // The deadline is already past at entry, but the stream's genuine EOF is
    // already queued. The pump must report the expiry it broke on — the
    // drained EOF must neither overwrite the outcome nor re-arm the chain.
    asio::io_context ioc;
    MockStream stream(ioc);
    stream.read_script.push_back({"", asio::error::eof});

    boost::system::error_code ec;
    bool delivered = false;
    testcontainers::docker::detail::duplex_exchange(
        ioc, stream, std::nullopt, "", /*eof_after_input=*/false,
        [&](const char*, std::size_t) {
            delivered = true;
            return true;
        },
        std::chrono::steady_clock::now() - std::chrono::milliseconds(1), [] {}, [] {}, ec);

    EXPECT_EQ(ec, asio::error::timed_out) << ec.message();
    EXPECT_FALSE(delivered);
    EXPECT_EQ(stream.reads_issued, 1); // never re-armed during the drain
}
