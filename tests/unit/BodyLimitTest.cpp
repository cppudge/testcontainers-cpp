#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "CannedHttpServer.hpp"
#include "TestSupport.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerClient.hpp"

// Tests in this file:
//   BodyLimit.DefaultIsUnlimited - with no cap set, a >1 MiB response body (over Beast's stock limit) is read in full.
//   BodyLimit.CapExceededThrows - set_max_response_body turns an oversized reply into a DockerError naming the cap; a response within the cap still passes.
//   BodyLimit.CopyCarriesCap - a copy of the client inherits the cap; so does a move.

using testcontainers::DockerClient;
using testcontainers::DockerError;

TEST(BodyLimit, DefaultIsUnlimited) {
    // 1.5 MiB — over Beast's stock 1 MiB parser limit, which request()
    // replaces with "none" when no cap is configured.
    const std::string big(std::size_t{3} * 512 * 1024, 'x');
    tcunit::CannedHttpServer server(tcunit::http_response(200, "OK", big));
    DockerClient client{server.host()};

    const testcontainers::Response res = client.request("GET", "/_ping");
    EXPECT_EQ(res.status_code, 200);
    EXPECT_EQ(res.body.size(), big.size());
}

TEST(BodyLimit, CapExceededThrows) {
    const std::string big(std::size_t{64} * 1024, 'x');
    tcunit::CannedHttpServer server(std::vector<std::string>{
        tcunit::http_response(200, "OK", big),
        tcunit::http_response(200, "OK", "small"),
    });
    DockerClient client{server.host()};
    client.set_max_response_body(1024);

    try {
        client.request("GET", "/_ping");
        FAIL() << "request did not throw on a body over the cap";
    } catch (const DockerError& e) {
        EXPECT_TRUE(tcunit::contains(e.what(), "max_response_body")) << e.what();
        EXPECT_TRUE(tcunit::contains(e.what(), "1024")) << e.what();
    }

    // A reply within the cap still passes on the same client.
    EXPECT_EQ(client.request("GET", "/_ping").body, "small");
}

TEST(BodyLimit, CopyCarriesCap) {
    const std::string big(std::size_t{64} * 1024, 'x');
    tcunit::CannedHttpServer server(tcunit::http_response(200, "OK", big));
    DockerClient client{server.host()};
    client.set_max_response_body(1024);

    const DockerClient copy = client;
    EXPECT_EQ(copy.max_response_body(), std::optional<std::uint64_t>{1024});

    DockerClient moved = std::move(client);
    EXPECT_EQ(moved.max_response_body(), std::optional<std::uint64_t>{1024});
    EXPECT_THROW(moved.request("GET", "/_ping"), DockerError);
}
