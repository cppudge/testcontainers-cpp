#pragma once

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tcit {

/// Send a minimal MQTT 3.1.1 CONNECT (clean session, anonymous, client id
/// "tc") over a raw TCP connection and return the broker's CONNACK — four
/// bytes `0x20 0x02 <session-present> <return-code>` — or whatever shorter
/// prefix arrived when the read fell short. Return code 0x00 = accepted,
/// 0x05 = not authorized (the broker sends the refusal CONNACK before
/// closing). The end-to-end reachability probe for every test that publishes
/// an MQTT port.
inline std::string mqtt_connect(const std::string& host, std::uint16_t port) {
    namespace asio = boost::asio;
    using asio::ip::tcp;

    asio::io_context io;
    tcp::resolver resolver(io);
    const auto endpoints = resolver.resolve(host, std::to_string(port));

    tcp::socket socket(io);
    asio::connect(socket, endpoints);

    // Fixed header (CONNECT, remaining length 14) + variable header
    // ("MQTT", level 4, clean-session flags, keepalive 60s) + payload
    // (client id "tc").
    static constexpr std::array<unsigned char, 16> connect_packet = {
        0x10, 0x0E, 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02, 0x00, 0x3C, 0x00, 0x02, 't', 'c'};
    asio::write(socket, asio::buffer(connect_packet));

    std::array<char, 4> ack{};
    boost::system::error_code ec;
    const std::size_t n = asio::read(socket, asio::buffer(ack), ec);
    return std::string(ack.data(), n);
}

} // namespace tcit
