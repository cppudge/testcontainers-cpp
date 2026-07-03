#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <string>
#include <thread>
#include <type_traits>

#include "docker/ApiMapping.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/DockerHost.hpp"

// Tests in this file (the typed error hierarchy — no Docker daemon; the HTTP
// tests use a canned loopback responder):
//   ErrorModel.HierarchyRelations - NotFoundError/TimeoutError ARE DockerError; StartupTimeoutError is Error but NOT DockerError (readiness != daemon failure).
//   ErrorModel.DockerErrorFieldDefaults - a message-only DockerError has status_code()==nullopt and an empty resource_id().
//   ErrorModel.NotFoundCarries404 - NotFoundError always reports status_code()==404 plus the resource id it was built with.
//   ErrorModel.GuardedParsersWrapInvalidJson - every parse entry point (inspect/list/volume/version/exec) turns an HTML-through-200 body into a DockerError naming its operation, never a raw json exception.
//   ErrorModel.GuardedParsersWrapTypeErrors - a syntactically-valid body with a wrong-typed field (Labels value not a string) is wrapped the same way.
//   ErrorModel.OversizedBodyIsTruncatedInMessage - a megabyte-scale garbage body is excerpted in the error text, not embedded whole.
//   ErrorModel.Http404BecomesNotFoundError - inspect_container against a 404-responding daemon throws NotFoundError with status 404 and the container id as resource_id (and is catchable as DockerError).
//   ErrorModel.Http500KeepsStatusOnDockerError - a 500 reply surfaces as a plain DockerError carrying status_code()==500.
//   ErrorModel.HtmlThrough200BecomesDockerError - a 200 reply with an HTML body makes inspect_container throw the guarded-parse DockerError instead of json::parse_error.

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

using testcontainers::DockerError;
using testcontainers::DockerHost;
using testcontainers::Error;
using testcontainers::NotFoundError;
using testcontainers::StartupTimeoutError;
using testcontainers::TimeoutError;

/// A loopback HTTP responder: accepts ONE connection, reads until the end of
/// the request head, writes `response` verbatim, and closes. Exactly enough
/// server to drive DockerClient's status/parse error paths.
class CannedHttpServer {
public:
    explicit CannedHttpServer(std::string response)
        : acceptor_(ioc_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          port_(acceptor_.local_endpoint().port()),
          thread_([this, response = std::move(response)] {
              boost::system::error_code ec;
              tcp::socket socket(ioc_);
              acceptor_.accept(socket, ec);
              if (ec) {
                  return; // destroyed before a client connected
              }
              // Read until the blank line ending the request head (our requests
              // carry no body worth waiting for beyond that).
              std::string request;
              char buf[1024];
              while (request.find("\r\n\r\n") == std::string::npos) {
                  const std::size_t n = socket.read_some(asio::buffer(buf), ec);
                  if (ec) {
                      break;
                  }
                  request.append(buf, n);
              }
              asio::write(socket, asio::buffer(response), ec);
              boost::system::error_code ignore;
              socket.shutdown(tcp::socket::shutdown_both, ignore);
              socket.close(ignore);
          }) {}

    ~CannedHttpServer() {
        boost::system::error_code ignore;
        acceptor_.close(ignore);
        thread_.join();
    }

    DockerHost host() const {
        return DockerHost::parse("tcp://127.0.0.1:" + std::to_string(port_));
    }

private:
    asio::io_context ioc_;
    tcp::acceptor acceptor_;
    std::uint16_t port_;
    std::thread thread_;
};

std::string http_response(int status, const std::string& reason, const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status) + " " + reason +
           "\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n\r\n" + body;
}

} // namespace

TEST(ErrorModel, HierarchyRelations) {
    static_assert(std::is_base_of_v<DockerError, NotFoundError>);
    static_assert(std::is_base_of_v<DockerError, TimeoutError>);
    static_assert(std::is_base_of_v<Error, StartupTimeoutError>);
    // Readiness timeouts are NOT daemon failures: catch(DockerError) must skip them.
    static_assert(!std::is_base_of_v<DockerError, StartupTimeoutError>);

    // The same relations at catch time.
    EXPECT_THROW(throw NotFoundError("gone"), DockerError);
    EXPECT_THROW(throw TimeoutError("slow"), DockerError);
    EXPECT_THROW(throw StartupTimeoutError("not ready"), Error);
    try {
        throw StartupTimeoutError("not ready");
    } catch (const DockerError&) {
        FAIL() << "StartupTimeoutError must not be catchable as DockerError";
    } catch (const Error&) {
        // expected
    }
}

TEST(ErrorModel, DockerErrorFieldDefaults) {
    const DockerError plain("boom");
    EXPECT_EQ(plain.status_code(), std::nullopt);
    EXPECT_EQ(plain.resource_id(), "");
    EXPECT_STREQ(plain.what(), "boom");

    const DockerError full("boom", 409, "abc123");
    EXPECT_EQ(full.status_code(), 409);
    EXPECT_EQ(full.resource_id(), "abc123");
}

TEST(ErrorModel, NotFoundCarries404) {
    const NotFoundError e("no such container", "deadbeef");
    EXPECT_EQ(e.status_code(), 404);
    EXPECT_EQ(e.resource_id(), "deadbeef");

    const TimeoutError t("timed out", "deadbeef");
    EXPECT_EQ(t.status_code(), std::nullopt); // a timeout never saw a status
    EXPECT_EQ(t.resource_id(), "deadbeef");
}

TEST(ErrorModel, GuardedParsersWrapInvalidJson) {
    const std::string html = "<html><body>502 Bad Gateway</body></html>";

    const auto expect_wrapped = [&](auto fn, const std::string& context) {
        try {
            fn();
            FAIL() << context << ": expected DockerError";
        } catch (const DockerError& e) {
            EXPECT_NE(std::string(e.what()).find(context), std::string::npos) << e.what();
            EXPECT_NE(std::string(e.what()).find("unexpected response from Docker"),
                      std::string::npos)
                << e.what();
        }
    };

    namespace docker = testcontainers::docker;
    expect_wrapped([&] { docker::parse_inspect(html); }, "inspect_container");
    expect_wrapped([&] { docker::parse_container_list(html); }, "list_containers");
    expect_wrapped([&] { docker::parse_volume_inspect(html); }, "inspect_volume");
    expect_wrapped([&] { docker::parse_server_os(html); }, "server_os");
    expect_wrapped([&] { docker::parse_exec_exit_code(html); }, "exec inspect");
}

TEST(ErrorModel, GuardedParsersWrapTypeErrors) {
    // Valid JSON, wrong shape: a numeric Labels value where a string is expected.
    const std::string bad_labels = R"({"Name":"v1","Labels":{"key":42}})";
    try {
        testcontainers::docker::parse_volume_inspect(bad_labels);
        FAIL() << "expected DockerError";
    } catch (const DockerError& e) {
        EXPECT_NE(std::string(e.what()).find("inspect_volume"), std::string::npos) << e.what();
    }
}

TEST(ErrorModel, OversizedBodyIsTruncatedInMessage) {
    const std::string huge = "<" + std::string(1 << 20, 'x'); // 1 MiB of not-JSON
    try {
        testcontainers::docker::parse_inspect(huge);
        FAIL() << "expected DockerError";
    } catch (const DockerError& e) {
        const std::string what = e.what();
        EXPECT_LT(what.size(), 4096u) << "the 1 MiB body leaked into the message";
        EXPECT_NE(what.find("truncated"), std::string::npos) << what;
    }
}

TEST(ErrorModel, Http404BecomesNotFoundError) {
    CannedHttpServer server(
        http_response(404, "Not Found", R"({"message":"No such container: deadbeef"})"));
    testcontainers::DockerClient client{server.host()};

    try {
        client.inspect_container("deadbeef");
        FAIL() << "expected NotFoundError";
    } catch (const NotFoundError& e) {
        EXPECT_EQ(e.status_code(), 404);
        EXPECT_EQ(e.resource_id(), "deadbeef");
        EXPECT_NE(std::string(e.what()).find("deadbeef"), std::string::npos) << e.what();
    }
}

TEST(ErrorModel, Http500KeepsStatusOnDockerError) {
    CannedHttpServer server(
        http_response(500, "Internal Server Error", R"({"message":"boom"})"));
    testcontainers::DockerClient client{server.host()};

    try {
        client.inspect_container("deadbeef");
        FAIL() << "expected DockerError";
    } catch (const NotFoundError&) {
        FAIL() << "a 500 must not be NotFoundError";
    } catch (const DockerError& e) {
        EXPECT_EQ(e.status_code(), 500);
        EXPECT_EQ(e.resource_id(), "deadbeef");
    }
}

TEST(ErrorModel, HtmlThrough200BecomesDockerError) {
    CannedHttpServer server(
        http_response(200, "OK", "<html><body>captive portal says hi</body></html>"));
    testcontainers::DockerClient client{server.host()};

    try {
        client.inspect_container("deadbeef");
        FAIL() << "expected DockerError";
    } catch (const DockerError& e) {
        EXPECT_NE(std::string(e.what()).find("unexpected response from Docker"),
                  std::string::npos)
            << e.what();
    }
}
