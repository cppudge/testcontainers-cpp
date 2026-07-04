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
/// connection carried it. Exactly enough server to drive DockerClient's
/// status/parse error paths, the start orchestration, and keep-alive reuse.
///
/// Declare the server BEFORE the client / session that talks to it: teardown
/// only unblocks a pending accept, so a thread blocked reading the next
/// scripted request on a still-cached connection relies on the client being
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
        : acceptor_(ioc_, boost::asio::ip::tcp::endpoint(
                              boost::asio::ip::make_address("127.0.0.1"), 0)),
          port_(acceptor_.local_endpoint().port()),
          thread_([this, connections = std::move(connections)] {
              namespace asio = boost::asio;
              for (const std::vector<std::string>& responses : connections) {
                  boost::system::error_code ec;
                  asio::ip::tcp::socket socket(ioc_);
                  acceptor_.accept(socket, ec);
                  if (ec || stop_) {
                      return; // destroyed before every connection was made
                  }
                  {
                      const std::lock_guard<std::mutex> lock(mutex_);
                      requests_by_connection_.emplace_back();
                  }
                  for (const std::string& response : responses) {
                      // Read the request head, then drain the body per its
                      // Content-Length. Draining matters: closing the socket
                      // with unread request bytes pending makes the OS send
                      // RST, which kills the response in flight (bites on
                      // bodied PUTs/POSTs).
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
                      // Hazard: a request announcing MORE Content-Length than
                      // the client actually sends deadlocks here (we block
                      // reading, the client blocks waiting for the response).
                      // Our client always sends exactly what it announces, and
                      // chunked encoding is not supported.
                      const std::size_t total = head_end + 4 + content_length(request);
                      while (!ec && request.size() < total) {
                          const std::size_t n = socket.read_some(asio::buffer(buf), ec);
                          request.append(buf, n);
                      }
                      {
                          const std::lock_guard<std::mutex> lock(mutex_);
                          requests_.push_back(request);
                          requests_by_connection_.back().push_back(request);
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
          }) {}

    ~CannedHttpServer() {
        // Unblock a pending accept with a throwaway connection instead of
        // closing the acceptor under the server thread's feet (concurrent ops
        // on one Asio object from two threads are undefined). The thread sees
        // stop_ right after its accept returns and exits.
        stop_ = true;
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
    static std::vector<std::vector<std::string>> to_connections(
        std::vector<std::string> responses) {
        std::vector<std::vector<std::string>> connections;
        connections.reserve(responses.size());
        for (std::string& response : responses) {
            connections.push_back({std::move(response)});
        }
        return connections;
    }

    /// The Content-Length announced in `head`, or 0 when absent (our client
    /// always sends it on bodied requests; chunked encoding is not supported).
    static std::size_t content_length(const std::string& head) {
        // Case-insensitive scan for the header name at a line start.
        static constexpr const char* kName = "content-length:";
        for (std::size_t pos = head.find("\r\n"); pos != std::string::npos;
             pos = head.find("\r\n", pos + 2)) {
            const std::size_t line = pos + 2;
            std::size_t i = 0;
            while (kName[i] != '\0' && line + i < head.size() &&
                   std::tolower(static_cast<unsigned char>(head[line + i])) == kName[i]) {
                ++i;
            }
            if (kName[i] == '\0') {
                return static_cast<std::size_t>(
                    std::strtoull(head.c_str() + line + i, nullptr, 10));
            }
        }
        return 0;
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
inline std::string http_response(int status, const std::string& reason,
                                 const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason +
           "\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

/// A body-less response (204/304 style — no Content-Length on purpose).
inline std::string http_response_no_body(int status, const std::string& reason) {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n\r\n";
}

} // namespace tcunit
