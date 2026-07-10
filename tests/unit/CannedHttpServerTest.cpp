#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "CannedHttpServer.hpp"

// Tests in this file:
//   CannedHttpServer.DecodesChunkedBody - a chunked PUT split awkwardly across writes (chunk extension included) is drained and recorded as head + DECODED payload, and the scripted response still arrives.
//   CannedHttpServer.ChunkedTerminatorSplitAcrossReads - the "0\r\n\r\n" terminator arriving one fragment at a time still ends the drain; the per-connection record carries the decoded body too.
//   CannedHttpServer.EmptyChunkedBody - a chunked body with no data chunks records just the head.
//   CannedHttpServer.ContentLengthBodyRecordedVerbatim - the Content-Length drain still records the raw request (head + body) unchanged.

namespace {

/// Send `writes` back-to-back on one connection to the server's port and read
/// the response until the server closes the connection.
std::string roundtrip(std::uint16_t port, const std::vector<std::string>& writes) {
    namespace asio = boost::asio;
    asio::io_context ioc;
    asio::ip::tcp::socket socket(ioc);
    socket.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    for (const std::string& piece : writes) {
        asio::write(socket, asio::buffer(piece));
    }

    std::string response;
    boost::system::error_code ec;
    char buf[1024];
    while (!ec) {
        const std::size_t n = socket.read_some(asio::buffer(buf), ec);
        response.append(buf, n);
    }
    return response;
}

} // namespace

TEST(CannedHttpServer, DecodesChunkedBody) {
    tcunit::CannedHttpServer server(tcunit::http_response(200, "OK", "{}"));

    const std::string head = "PUT /containers/x/archive?path=/ HTTP/1.1\r\n"
                             "Host: 127.0.0.1\r\n"
                             "Content-Type: application/x-tar\r\n"
                             "Transfer-Encoding: chunked\r\n"
                             "\r\n";
    // Split so chunk-size lines and chunk data straddle read boundaries; the
    // first chunk carries an extension (";ext=1") the decoder must skip.
    const std::string response = roundtrip(server.host().port(), {
                                                                     head,
                                                                     "5;ext=1\r\nhel",
                                                                     "lo\r\n6\r\n wor",
                                                                     "ld\r\n0\r\n\r\n",
                                                                 });
    EXPECT_NE(response.find("200 OK"), std::string::npos);

    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 1u);
    // Recorded DECODED: the head verbatim (Transfer-Encoding still visible for
    // tests asserting the encoding), then the payload without chunk framing.
    EXPECT_EQ(requests[0], head + "hello world");
}

TEST(CannedHttpServer, ChunkedTerminatorSplitAcrossReads) {
    tcunit::CannedHttpServer server(tcunit::http_response(200, "OK", "{}"));

    const std::string head = "PUT /x HTTP/1.1\r\n"
                             "Host: 127.0.0.1\r\n"
                             "Transfer-Encoding: chunked\r\n"
                             "\r\n";
    const std::string response = roundtrip(server.host().port(), {
                                                                     head,
                                                                     "3\r\nabc\r\n",
                                                                     "0\r",
                                                                     "\n\r",
                                                                     "\n",
                                                                 });
    EXPECT_NE(response.find("200 OK"), std::string::npos);

    const std::vector<std::vector<std::string>> by_conn = server.requests_by_connection();
    ASSERT_EQ(by_conn.size(), 1u);
    ASSERT_EQ(by_conn[0].size(), 1u);
    EXPECT_EQ(by_conn[0][0], head + "abc"); // decoded in the per-connection record too
}

TEST(CannedHttpServer, EmptyChunkedBody) {
    tcunit::CannedHttpServer server(tcunit::http_response(200, "OK", "{}"));

    const std::string head = "PUT /x HTTP/1.1\r\n"
                             "Host: 127.0.0.1\r\n"
                             "Transfer-Encoding: chunked\r\n"
                             "\r\n";
    const std::string response = roundtrip(server.host().port(), {head + "0\r\n\r\n"});
    EXPECT_NE(response.find("200 OK"), std::string::npos);

    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0], head);
}

TEST(CannedHttpServer, ContentLengthBodyRecordedVerbatim) {
    tcunit::CannedHttpServer server(tcunit::http_response(200, "OK", "{}"));

    const std::string request = "POST /volumes/create HTTP/1.1\r\n"
                                "Host: 127.0.0.1\r\n"
                                "Content-Length: 4\r\n"
                                "\r\n"
                                "abcd";
    const std::string response = roundtrip(server.host().port(), {request});
    EXPECT_NE(response.find("200 OK"), std::string::npos);

    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0], request);
}
