#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <vector>

#include "CannedHttpServer.hpp"
#include "TestSupport.hpp"
#include "docker/ApiMapping.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/RegistryAuth.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/DockerHost.hpp"

// Tests in this file (the typed error hierarchy — no Docker daemon; the HTTP
// tests use a canned loopback responder):
//   ErrorModel.HierarchyRelations - NotFoundError/TransportTimeoutError ARE DockerError; StartupTimeoutError is Error but NOT DockerError (readiness != daemon failure).
//   ErrorModel.DockerErrorFieldDefaults - a message-only DockerError has status_code()==nullopt and an empty resource_id().
//   ErrorModel.NotFoundCarries404 - NotFoundError always reports status_code()==404 plus the resource id it was built with.
//   ErrorModel.StartupTimeoutCarriesResourceId - StartupTimeoutError exposes the container id / compose service it was built with.
//   ErrorModel.GuardedParsersWrapInvalidJson - every parse entry point (inspect/list/volume/version/exec) turns an HTML-through-200 body into a DockerError naming its operation, never a raw json exception.
//   ErrorModel.GuardedParsersWrapTypeErrors - a syntactically-valid body with a wrong-typed field (Labels value not a string) is wrapped the same way.
//   ErrorModel.PullStreamErrorCarriesImage - the in-stream pull error (daemon streams it through a 200) carries the image as resource_id.
//   ErrorModel.OversizedBodyIsTruncatedInMessage - a megabyte-scale garbage body is excerpted in the error text, not embedded whole.
//   ErrorModel.Http404BecomesNotFoundError - inspect_container against a 404-responding daemon throws NotFoundError with status 404 and the container id as resource_id (and is catchable as DockerError).
//   ErrorModel.Http500KeepsStatusOnDockerError - a 500 reply surfaces as a plain DockerError carrying status_code()==500.
//   ErrorModel.HtmlThrough200BecomesDockerError - a 200 reply with an HTML body makes inspect_container throw the guarded-parse DockerError instead of json::parse_error.
//   ErrorModel.CreateContainer404PullsAndRetries - create_container's 404 -> pull -> retry sequence succeeds against a canned 404/200/201 daemon (explicit auth skips the host's cred-helper lookup).

namespace {

using tcunit::CannedHttpServer;
using tcunit::http_response;
using tcunit::ping_ok;

using testcontainers::DockerError;
using testcontainers::Error;
using testcontainers::NotFoundError;
using testcontainers::StartupTimeoutError;
using testcontainers::TransportTimeoutError;

} // namespace

TEST(ErrorModel, HierarchyRelations) {
    static_assert(std::is_base_of_v<DockerError, NotFoundError>);
    static_assert(std::is_base_of_v<DockerError, TransportTimeoutError>);
    static_assert(std::is_base_of_v<Error, StartupTimeoutError>);
    // Readiness timeouts are NOT daemon failures: catch(DockerError) must skip them.
    static_assert(!std::is_base_of_v<DockerError, StartupTimeoutError>);

    // The same relations at catch time.
    EXPECT_THROW(throw NotFoundError("gone"), DockerError);
    EXPECT_THROW(throw TransportTimeoutError("slow"), DockerError);
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

    const TransportTimeoutError t("timed out", "deadbeef");
    EXPECT_EQ(t.status_code(), std::nullopt); // a timeout never saw a status
    EXPECT_EQ(t.resource_id(), "deadbeef");
}

TEST(ErrorModel, StartupTimeoutCarriesResourceId) {
    const StartupTimeoutError e("container abc did not become ready", "abc");
    EXPECT_EQ(e.resource_id(), "abc");
    EXPECT_EQ(StartupTimeoutError("no subject").resource_id(), "");
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

TEST(ErrorModel, PullStreamErrorCarriesImage) {
    // The daemon reports pull failures through a 200 as an in-stream "error"
    // line — the most common "image doesn't exist" surface must still carry
    // the image as resource_id.
    const std::string stream =
        "{\"status\":\"Pulling\"}\n{\"error\":\"pull access denied for nope\"}\n";
    try {
        testcontainers::docker::throw_if_pull_error(stream, "nope:latest");
        FAIL() << "expected DockerError";
    } catch (const DockerError& e) {
        EXPECT_EQ(e.resource_id(), "nope:latest");
        EXPECT_EQ(e.status_code(), std::nullopt);
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
    CannedHttpServer server(std::vector<std::string>{
        ping_ok(),
        http_response(404, "Not Found", R"({"message":"No such container: deadbeef"})")});
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
    CannedHttpServer server(std::vector<std::string>{
        ping_ok(), http_response(500, "Internal Server Error", R"({"message":"boom"})")});
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
    CannedHttpServer server(std::vector<std::string>{
        ping_ok(), http_response(200, "OK", "<html><body>captive portal says hi</body></html>")});
    testcontainers::DockerClient client{server.host()};

    try {
        client.inspect_container("deadbeef");
        FAIL() << "expected DockerError";
    } catch (const DockerError& e) {
        EXPECT_NE(std::string(e.what()).find("unexpected response from Docker"), std::string::npos)
            << e.what();
    }
}

TEST(ErrorModel, CreateContainer404PullsAndRetries) {
    CannedHttpServer server({
        // 0) GET /_ping -> the one-time API-version negotiation
        ping_ok(),
        // 1) POST /containers/create -> 404 (image not present locally)
        http_response(404, "Not Found", R"({"message":"No such image: busybox:latest"})"),
        // 2) POST /images/create -> 200 with a clean progress stream
        http_response(200, "OK", "{\"status\":\"Pulling from library/busybox\"}\n"),
        // 3) POST /containers/create retry -> 201
        http_response(201, "Created", R"({"Id":"abc123"})"),
    });
    testcontainers::DockerClient client{server.host()};

    testcontainers::CreateContainerSpec spec;
    spec.image = "busybox:latest";
    // Explicit (empty) credentials keep the pull path off this host's Docker
    // config / credential helpers — the unit test must not shell out.
    const std::string id = client.create_container(spec, testcontainers::RegistryAuth{});
    EXPECT_EQ(id, "abc123");
}
