#pragma once

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <string>

namespace tcit {

/// Plain HTTP/1.1 GET against a published port; returns the whole response
/// (status line + headers + body). The end-to-end reachability probe for
/// tests that publish an HTTP-speaking port — a bare TCP connect proves
/// nothing against Docker Desktop's accept-anything host proxy.
inline std::string http_get(const std::string& host, std::uint16_t port,
                            const std::string& target) {
    namespace asio = boost::asio;
    using asio::ip::tcp;

    asio::io_context io;
    tcp::resolver resolver(io);
    tcp::socket socket(io);
    asio::connect(socket, resolver.resolve(host, std::to_string(port)));

    const std::string request =
        "GET " + target + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    asio::write(socket, asio::buffer(request));

    std::string response;
    boost::system::error_code ec;
    asio::read(socket, asio::dynamic_buffer(response), ec); // drain to EOF
    return response;
}

} // namespace tcit
