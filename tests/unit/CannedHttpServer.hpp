#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "testcontainers/docker/DockerHost.hpp"

namespace tcunit {

/// A loopback HTTP responder serving a canned script: one entry per accepted
/// CONNECTION, each entry a list of responses served back-to-back on that
/// connection (read a request, write the next response, repeat, then close).
/// The single-level constructors keep the historical one-connection-per-
/// response behavior. It records each request (head + body) globally and per
/// connection, so tests can assert both the call sequence and which
/// connection carried it; a chunked request body is drained and recorded
/// DECODED (the head keeps its Transfer-Encoding header). Exactly enough
/// server to drive DockerClient's status/parse error paths, the start
/// orchestration, keep-alive reuse, and the chunked streaming uploads.
///
/// Connections are served CONCURRENTLY (a worker thread per accepted
/// connection): exec opens its hijack transport first but sends its request
/// only after a create round-trip on a SECOND connection, so a serial server
/// would deadlock. Script entries are still matched to connections in accept
/// order.
///
/// Declare the server BEFORE the client / session that talks to it: teardown
/// only unblocks a pending accept, so a worker blocked reading the next
/// scripted request on a still-open connection relies on the client being
/// destroyed (closing that connection) first.
class CannedHttpServer {
public:
    explicit CannedHttpServer(std::string response)
        : CannedHttpServer(std::vector<std::string>{std::move(response)}) {}

    /// One connection per response (each connection serves a single exchange).
    explicit CannedHttpServer(std::vector<std::string> responses)
        : CannedHttpServer(to_connections(std::move(responses))) {}

    /// Full script: connections[i] is the ordered response list served on the
    /// i-th accepted connection.
    explicit CannedHttpServer(std::vector<std::vector<std::string>> connections)
        : acceptor_(ioc_,
                    boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0)),
          port_(acceptor_.local_endpoint().port()),
          thread_([this, connections = std::move(connections)] {
              namespace asio = boost::asio;
              std::vector<std::thread> workers;
              for (std::size_t i = 0; i < connections.size(); ++i) {
                  boost::system::error_code ec;
                  asio::ip::tcp::socket socket(ioc_);
                  acceptor_.accept(socket, ec);
                  if (ec || stop_) {
                      break; // destroyed before every connection was made
                  }
                  {
                      const std::lock_guard<std::mutex> lock(mutex_);
                      requests_by_connection_.emplace_back();
                  }
                  // Serve each connection on its own worker: exec-shaped
                  // clients hold one connection open while doing a full
                  // round-trip on another, which a serial loop would deadlock
                  // on. `connections` outlives the workers (joined below).
                  workers.emplace_back([this, socket = std::move(socket),
                                        &responses = connections[i],
                                        i]() mutable { serve_connection(socket, responses, i); });
              }
              for (std::thread& worker : workers) {
                  worker.join();
              }
          }) {}

    ~CannedHttpServer() {
        // Unblock a pending accept with a throwaway connection instead of
        // closing the acceptor under the server thread's feet (concurrent ops
        // on one Asio object from two threads are undefined). The thread sees
        // stop_ right after its accept returns and exits.
        stop_ = true;
        try {
            {
                boost::asio::io_context poke_io;
                boost::asio::ip::tcp::socket poke(poke_io);
                boost::system::error_code ignore;
                poke.connect(boost::asio::ip::tcp::endpoint(
                                 boost::asio::ip::make_address("127.0.0.1"), port_),
                             ignore);
            }
            thread_.join();
            boost::system::error_code ignore;
            acceptor_.close(ignore);
        } catch (...) {
            // Best-effort: a destructor must never throw (join only throws
            // when the thread is already unjoinable).
        }
    }

    testcontainers::DockerHost host() const {
        return testcontainers::DockerHost::parse("tcp://127.0.0.1:" + std::to_string(port_));
    }

    /// The raw requests (head + body) served so far, in order.
    std::vector<std::string> requests() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    /// The raw requests grouped by the connection that carried them, in accept
    /// order (an accepted connection appears even if it served no request).
    std::vector<std::vector<std::string>> requests_by_connection() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_by_connection_;
    }

private:
    /// The per-connection script runner (one worker thread each): read a
    /// request, write the next response, repeat, then close.
    void serve_connection(boost::asio::ip::tcp::socket& socket,
                          const std::vector<std::string>& responses, std::size_t index) {
        namespace asio = boost::asio;
        boost::system::error_code ec;
        for (const std::string& response : responses) {
            // Read the request head, then drain the body per its
            // Content-Length. Draining matters: closing the socket with
            // unread request bytes pending makes the OS send RST, which
            // kills the response in flight (bites on bodied PUTs/POSTs).
            std::string request;
            char buf[1024];
            while (request.find("\r\n\r\n") == std::string::npos) {
                const std::size_t n = socket.read_some(asio::buffer(buf), ec);
                if (ec) {
                    break;
                }
                request.append(buf, n);
            }
            const std::size_t head_end = request.find("\r\n\r\n");
            if (head_end == std::string::npos) {
                break; // client closed / gave up on this connection
            }
            const std::string head = request.substr(0, head_end + 4);
            std::string recorded;
            if (is_chunked(head)) {
                // A chunked body (the client's streaming uploads): drain and
                // DECODE it, so the recorded request carries the payload
                // bytes without chunk framing — tests assert on the payload,
                // and the head keeps its Transfer-Encoding header for tests
                // that assert the encoding itself.
                recorded = head + drain_chunked_body(socket, request, head_end + 4, ec);
            } else {
                // Hazard: a request announcing MORE Content-Length than the
                // client actually sends deadlocks here (we block reading, the
                // client blocks waiting for the response). Our client always
                // sends exactly what it announces.
                const std::size_t total = head_end + 4 + content_length(head);
                while (!ec && request.size() < total) {
                    const std::size_t n = socket.read_some(asio::buffer(buf), ec);
                    request.append(buf, n);
                }
                recorded = request;
            }
            {
                // Indexed access under the lock: the accept loop may still be
                // growing the vector (reallocation), so no reference is held
                // across the append.
                const std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back(recorded);
                requests_by_connection_[index].push_back(recorded);
            }
            asio::write(socket, asio::buffer(response), ec);
            if (ec) {
                break;
            }
        }
        boost::system::error_code ignore;
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
        socket.close(ignore);
    }

    static std::vector<std::vector<std::string>>
    to_connections(std::vector<std::string> responses) {
        std::vector<std::vector<std::string>> connections;
        connections.reserve(responses.size());
        for (std::string& response : responses) {
            connections.push_back({std::move(response)});
        }
        return connections;
    }

    /// The value of the `name` header (pass it lowercase WITH the colon, e.g.
    /// "content-length:") in `head`, or "" when absent. Case-insensitive scan
    /// for the header name at a line start.
    static std::string find_header(const std::string& head, const char* name) {
        for (std::size_t pos = head.find("\r\n"); pos != std::string::npos;
             pos = head.find("\r\n", pos + 2)) {
            const std::size_t line = pos + 2;
            std::size_t i = 0;
            while (name[i] != '\0' && line + i < head.size() &&
                   std::tolower(static_cast<unsigned char>(head[line + i])) == name[i]) {
                ++i;
            }
            if (name[i] == '\0') {
                const std::size_t value = line + i;
                const std::size_t end = head.find("\r\n", value);
                return head.substr(value,
                                   end == std::string::npos ? std::string::npos : end - value);
            }
        }
        return {};
    }

    /// The Content-Length announced in `head`, or 0 when absent.
    static std::size_t content_length(const std::string& head) {
        const std::string value = find_header(head, "content-length:");
        return value.empty() ? 0
                             : static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
    }

    /// True when `head` announces a chunked request body.
    static bool is_chunked(const std::string& head) {
        std::string value = find_header(head, "transfer-encoding:");
        for (char& c : value) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return value.find("chunked") != std::string::npos;
    }

    /// Read and DECODE a chunked request body. `data` holds everything read
    /// off the connection so far (head included); the body starts at `pos`,
    /// and more is read from `socket` as needed. Returns the decoded payload;
    /// on a read error (`ec` set — client died mid-body) the decode stops
    /// early with what arrived.
    static std::string drain_chunked_body(boost::asio::ip::tcp::socket& socket, std::string& data,
                                          std::size_t pos, boost::system::error_code& ec) {
        namespace asio = boost::asio;
        char buf[1024];
        const auto read_more = [&]() -> bool {
            const std::size_t n = socket.read_some(asio::buffer(buf), ec);
            if (ec) {
                return false;
            }
            data.append(buf, n);
            return true;
        };
        const auto find_crlf = [&](std::size_t from) -> std::size_t {
            std::size_t at = 0;
            while ((at = data.find("\r\n", from)) == std::string::npos) {
                if (!read_more()) {
                    return std::string::npos;
                }
            }
            return at;
        };

        std::string body;
        for (;;) {
            // "<hex-size>[;extension]\r\n" — strtoull stops at the ';'.
            const std::size_t line_end = find_crlf(pos);
            if (line_end == std::string::npos) {
                return body;
            }
            const std::size_t size =
                static_cast<std::size_t>(std::strtoull(data.c_str() + pos, nullptr, 16));
            pos = line_end + 2;
            if (size == 0) {
                // Trailer section: lines until the empty terminator (our
                // client sends no trailers, so the next line IS the
                // terminator).
                for (;;) {
                    const std::size_t t = find_crlf(pos);
                    if (t == std::string::npos || t == pos) {
                        return body;
                    }
                    pos = t + 2;
                }
            }
            while (data.size() < pos + size + 2) { // the chunk + its trailing CRLF
                if (!read_more()) {
                    return body;
                }
            }
            body.append(data, pos, size);
            pos += size + 2;
        }
    }

    boost::asio::io_context ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
    std::vector<std::vector<std::string>> requests_by_connection_;
    std::thread thread_;
};

/// A full HTTP/1.1 response with a JSON content type and the right Content-Length.
inline std::string http_response(int status, const std::string& reason, const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason +
           "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + body;
}

/// A body-less response (204/304 style — no Content-Length on purpose).
inline std::string http_response_no_body(int status, const std::string& reason) {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n\r\n";
}

} // namespace tcunit
