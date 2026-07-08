#pragma once

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tcit {

/// Send a Redis PING over a raw TCP connection and return the reply (or "").
/// The end-to-end reachability probe for every test that publishes a redis
/// port.
inline std::string redis_ping(const std::string& host, std::uint16_t port) {
    namespace asio = boost::asio;
    using asio::ip::tcp;

    asio::io_context io;
    tcp::resolver resolver(io);
    const auto endpoints = resolver.resolve(host, std::to_string(port));

    tcp::socket socket(io);
    asio::connect(socket, endpoints);

    const std::string ping = "PING\r\n";
    asio::write(socket, asio::buffer(ping));

    std::array<char, 64> buf{};
    boost::system::error_code ec;
    const std::size_t n = socket.read_some(asio::buffer(buf), ec);
    return std::string(buf.data(), n);
}

} // namespace tcit
