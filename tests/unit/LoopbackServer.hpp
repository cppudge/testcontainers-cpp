#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "testcontainers/docker/DockerHost.hpp"

namespace tcunit {

/// Complete a still-pending blocking accept() on 127.0.0.1:`port`
/// deterministically with a throwaway connection. Closing the acceptor from
/// another thread does NOT unblock a synchronous accept (asio's sync accept
/// waits in the reactor, which a cross-thread close() never wakes on Linux) —
/// joining the server thread after that deadlocked whole suites about 1% of
/// the time. The throwaway connect works in every interleaving: either the
/// accept was pending (it completes with this connection) or a client already
/// connected (this lands in the backlog and is never accepted).
inline void wake_pending_accept(std::uint16_t port) {
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket poke(io);
    boost::system::error_code ignore;
    poke.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port),
                 ignore);
}

/// A loopback TCP server accepting ONE connection and running `session` on it.
/// The session owns the socket's fate: close it for a clean end, or return
/// and let the server hold it open — the shape of a silent/wedged peer —
/// until the server is destroyed. A `receive_buffer` shrinks SO_RCVBUF on the
/// LISTENER (set before listening starts; the accepted socket inherits it at
/// handshake time, and setting it from another thread would race the accept
/// loop) so a writer facing this peer runs out of in-flight room after a few
/// dozen KB — kernels otherwise absorb many megabytes on loopback.
///
/// Teardown note for sessions: the destructor completes a never-arrived
/// accept with a throwaway connection (see wake_pending_accept), so a session
/// may run against that throwaway socket — its reads/writes fail immediately,
/// which every session must tolerate (they all do: an error ends them).
class LoopbackServer {
public:
    using Session = std::function<void(boost::asio::ip::tcp::socket&)>;

    explicit LoopbackServer(Session session = {}, std::optional<int> receive_buffer = std::nullopt)
        : acceptor_(make_acceptor(ioc_, receive_buffer)), port_(acceptor_.local_endpoint().port()),
          thread_([this, session = std::move(session)] {
              boost::system::error_code ec;
              boost::asio::ip::tcp::socket socket(ioc_);
              acceptor_.accept(socket, ec);
              if (ec) {
                  return; // destroyed before a client connected
              }
              if (session) {
                  session(socket);
              }
              stop_.get_future().wait(); // hold whatever is left open, silently
              boost::system::error_code ignore;
              socket.close(ignore);
          }) {}

    ~LoopbackServer() {
        try {
            stop_.set_value();
            wake_pending_accept(port_);
            thread_.join();
        } catch (...) {
            // Best-effort: a destructor must never throw (join only throws
            // when the thread is already unjoinable).
        }
    }

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    std::uint16_t port() const noexcept { return port_; }

    testcontainers::DockerHost host() const {
        return testcontainers::DockerHost::parse("tcp://127.0.0.1:" + std::to_string(port_));
    }

private:
    static boost::asio::ip::tcp::acceptor make_acceptor(boost::asio::io_context& ioc,
                                                        std::optional<int> receive_buffer) {
        boost::asio::ip::tcp::acceptor acceptor(
            ioc, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
        if (receive_buffer) {
            boost::system::error_code ignore;
            acceptor.set_option(boost::asio::socket_base::receive_buffer_size(*receive_buffer),
                                ignore);
        }
        return acceptor;
    }

    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_;
    std::promise<void> stop_;
    std::thread thread_;
};

} // namespace tcunit
