#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "CannedHttpServer.hpp"
#include "Runner.hpp"
#include "testcontainers/Container.hpp"
#include "testcontainers/ContainerRequest.hpp"
#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/RegistryAuth.hpp"
#include "testcontainers/docker/DockerClient.hpp"

// Tests in this file (the start orchestration core, detail::Runner::run, driven
// against a canned loopback HTTP responder — no Docker daemon):
//   Runner.CreateStartWaitReturnsHandle - the happy path issues create -> start, the create body carries the managed-by session labels, and the auto-removing handle DELETEs the container on drop.
//   Runner.KeepSkipsRemovalOnDrop - keep() flips an auto-removing handle to persistent (is_persistent() reports true): its drop issues NO DELETE.
//   Runner.HooksFireInOrderAroundCopy - created/starting/started/stopping hooks fire in order with the container id, with the copy-to PUT between create and start.
//   Runner.FailedStartRemovesPartialContainer - a 500 on start propagates as DockerError(500) after the partial container is force-removed.
//   Runner.ThrowingCreatedHookRemovesContainer - an exception from a created hook aborts the run and still removes the partial container.
//   Runner.RetryCreatesFreshContainer - with startup_attempts=2 a failed first attempt is removed and a brand-new container is created and returned.
//   Runner.RetryExhaustionRethrowsLast - when every attempt fails, the LAST attempt's error propagates (each partial container removed).
//   Runner.PullPolicyAlwaysPullsBeforeCreate - ImagePullPolicy::Always issues POST /images/create before the container create.
//   Runner.WaitTimeoutRemovesContainerAndConsumesAttempts - a readiness timeout surfaces as StartupTimeoutError, force-removes the partial container, and consumes a startup attempt (it IS retried).
//   Runner.ReuseAdoptsRunningMatch - with reuse enabled, a running hash-match is adopted (no create, not retried) and the handle is persistent (no DELETE on drop).
//   Runner.ReuseCreateBodyCarriesHashNotSessionLabel - with reuse enabled and no match, the fresh container's create body carries the reuse-hash label and NEVER the session-id label (Ryuk must not reap it); the handle is persistent.

namespace {

using tcunit::CannedHttpServer;
using tcunit::http_response;
using tcunit::http_response_no_body;

using testcontainers::Container;
using testcontainers::ContainerRequest;
using testcontainers::CopyToContainer;
using testcontainers::DockerError;
using testcontainers::detail::Runner;

/// The API-version negotiation `GET /_ping` every fresh client issues before
/// its first typed call. No Api-Version header on purpose: the client then
/// falls back to unversioned paths, so the path assertions below stay
/// version-independent (the pinning itself is covered by ApiVersionTest).
std::string ping_ok() { return http_response(200, "OK", "OK"); }

std::string created(const std::string& id) {
    return http_response(201, "Created", R"({"Id":")" + id + R"("})");
}

std::string started() { return http_response_no_body(204, "No Content"); }

std::string removed() { return http_response_no_body(204, "No Content"); }

std::string server_error(const std::string& message, int status = 500) {
    return http_response(status, "Error", R"({"message":")" + message + R"("})");
}

ContainerRequest busybox_request() {
    ContainerRequest request;
    request.spec.image = "busybox:latest";
    return request;
}

/// True when the recorded request head starts with "<METHOD> <path-prefix>".
bool request_is(const std::string& head, const std::string& method_and_path) {
    return head.rfind(method_and_path, 0) == 0;
}

/// Sets an environment variable for the test's scope and restores the prior
/// value on destruction (tests run single-threaded; env is process-global).
class ScopedEnv {
public:
    ScopedEnv(std::string name, const char* value) : name_(std::move(name)) {
        if (const char* old = std::getenv(name_.c_str())) {
            old_ = old;
        }
        set(value);
    }
    ~ScopedEnv() { set(old_ ? old_->c_str() : nullptr); }

private:
    void set(const char* value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value ? value : ""); // empty value removes it
#else
        if (value) {
            setenv(name_.c_str(), value, 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

    std::string name_;
    std::optional<std::string> old_;
};

} // namespace

TEST(Runner, CreateStartWaitReturnsHandle) {
    CannedHttpServer server({ping_ok(), created("abc123"), started(), removed()});
    {
        testcontainers::DockerClient client{server.host()};
        const Container c = Runner::run(client, busybox_request());
        EXPECT_EQ(c.id(), "abc123");
        EXPECT_FALSE(c.is_persistent());
    } // drop -> DELETE

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 4u);
    EXPECT_TRUE(request_is(requests[0], "GET /_ping")) << requests[0];
    EXPECT_TRUE(request_is(requests[1], "POST /containers/create")) << requests[1];
    EXPECT_TRUE(request_is(requests[2], "POST /containers/abc123/start")) << requests[2];
    // The handle's client is a COPY, and copies inherit the negotiated API
    // version — the drop-time DELETE must not re-ping.
    EXPECT_TRUE(request_is(requests[3], "DELETE /containers/abc123")) << requests[3];

    // The normal path tags the create body so Ryuk (and tooling) can find the
    // container. (The session-id label depends on TESTCONTAINERS_RYUK_DISABLED,
    // so only the unconditional label is pinned here.)
    EXPECT_NE(requests[1].find("org.testcontainers.managed-by"), std::string::npos) << requests[1];
}

TEST(Runner, KeepSkipsRemovalOnDrop) {
    // The mirror of CreateStartWaitReturnsHandle's drop: after keep() the
    // handle is persistent, so going out of scope must NOT issue a DELETE.
    CannedHttpServer server({ping_ok(), created("abc123"), started()});
    {
        testcontainers::DockerClient client{server.host()};
        Container c = Runner::run(client, busybox_request());
        EXPECT_FALSE(c.is_persistent());
        c.keep();
        EXPECT_TRUE(c.is_persistent());
    } // drop -> nothing: keep() released removal ownership

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 3u); // ping, create, start — no DELETE on drop
    EXPECT_TRUE(request_is(requests[0], "GET /_ping")) << requests[0];
    EXPECT_TRUE(request_is(requests[1], "POST /containers/create")) << requests[1];
    EXPECT_TRUE(request_is(requests[2], "POST /containers/abc123/start")) << requests[2];
}

TEST(Runner, WaitTimeoutRemovesContainerAndConsumesAttempts) {
    const std::string no_logs = http_response(200, "OK", "");
    CannedHttpServer server({
        ping_ok(), // version negotiation, once per client
        created("one"),
        started(),
        no_logs,
        removed(), // attempt 1: wait times out
        created("two"),
        started(),
        no_logs,
        removed(), // attempt 2: same
    });
    testcontainers::DockerClient client{server.host()};

    ContainerRequest request = busybox_request();
    request.waits.push_back(testcontainers::wait_for::log("NEVER_LOGGED"));
    // A zero timeout expires right after the FIRST log fetch of each attempt,
    // keeping the polling request count deterministic (exactly one GET /logs).
    request.startup_timeout = std::chrono::milliseconds(0);
    request.startup_attempts = 2;

    try {
        Runner::run(client, request);
        FAIL() << "expected StartupTimeoutError";
    } catch (const testcontainers::StartupTimeoutError& e) {
        // A readiness timeout consumed BOTH attempts; the last one propagates.
        EXPECT_EQ(e.resource_id(), "two");
    }

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 9u);
    EXPECT_TRUE(request_is(requests[3], "GET /containers/one/logs")) << requests[3];
    EXPECT_TRUE(request_is(requests[4], "DELETE /containers/one")) << requests[4];
    EXPECT_TRUE(request_is(requests[8], "DELETE /containers/two")) << requests[8];
}

TEST(Runner, ReuseAdoptsRunningMatch) {
    const ScopedEnv enable("TESTCONTAINERS_REUSE_ENABLE", "1");
    CannedHttpServer server(
        {ping_ok(), http_response(200, "OK", R"([{"Id":"reused1","State":"running"}])")});

    ContainerRequest request = busybox_request();
    request.reuse = true;
    {
        testcontainers::DockerClient client{server.host()};
        const Container c = Runner::run(client, request);
        EXPECT_EQ(c.id(), "reused1");
        EXPECT_TRUE(c.is_persistent());
    } // a persistent handle must NOT issue a DELETE on drop

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2u); // ping + the label-filtered list; NO create
    EXPECT_TRUE(request_is(requests[1], "GET /containers/json")) << requests[1];
}

TEST(Runner, ReuseCreateBodyCarriesHashNotSessionLabel) {
    const ScopedEnv enable("TESTCONTAINERS_REUSE_ENABLE", "1");
    CannedHttpServer server({
        ping_ok(),
        http_response(200, "OK", "[]"), // no running match
        created("fresh1"),
        started(),
    });

    ContainerRequest request = busybox_request();
    request.reuse = true;
    {
        testcontainers::DockerClient client{server.host()};
        const Container c = Runner::run(client, request);
        EXPECT_EQ(c.id(), "fresh1");
        EXPECT_TRUE(c.is_persistent());
    }

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 4u); // ping, list, create, start — no DELETE on drop
    // The create body is tagged for reuse discovery but NEVER session-labeled
    // (Ryuk would reap the container it is supposed to outlive).
    EXPECT_NE(requests[2].find("org.testcontainers.reuse.hash"), std::string::npos) << requests[2];
    EXPECT_NE(requests[2].find("org.testcontainers.managed-by"), std::string::npos) << requests[2];
    EXPECT_EQ(requests[2].find("org.testcontainers.session-id"), std::string::npos) << requests[2];
}

TEST(Runner, HooksFireInOrderAroundCopy) {
    CannedHttpServer server({
        ping_ok(),
        created("abc123"),
        http_response(200, "OK", "{}"), // PUT /containers/abc123/archive
        started(),
        removed(),
    });
    std::vector<std::string> events;
    const auto record = [&](const char* name) {
        return [&events, name](testcontainers::DockerClient&, const std::string& id) {
            events.push_back(std::string(name) + ":" + id);
        };
    };

    ContainerRequest request = busybox_request();
    request.copy_to_sources.push_back(CopyToContainer::content("hello", "/tmp/hello.txt"));
    request.created_hooks.emplace_back(record("created"));
    request.starting_hooks.emplace_back(record("starting"));
    request.started_hooks.emplace_back(record("started"));
    request.stopping_hooks.emplace_back(record("stopping"));

    {
        testcontainers::DockerClient client{server.host()};
        const Container c = Runner::run(client, request);
        EXPECT_EQ(c.id(), "abc123");
    } // drop fires the stopping hook, then DELETEs

    EXPECT_EQ(events, (std::vector<std::string>{"created:abc123", "starting:abc123",
                                                "started:abc123", "stopping:abc123"}));

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 5u);
    // The copy-to PUT sits between create and start (created hook -> copy ->
    // starting hook -> start).
    EXPECT_TRUE(request_is(requests[2], "PUT /containers/abc123/archive")) << requests[2];
    EXPECT_TRUE(request_is(requests[3], "POST /containers/abc123/start")) << requests[3];
}

TEST(Runner, FailedStartRemovesPartialContainer) {
    CannedHttpServer server({ping_ok(), created("abc123"), server_error("boom"), removed()});
    testcontainers::DockerClient client{server.host()};

    try {
        Runner::run(client, busybox_request());
        FAIL() << "expected DockerError";
    } catch (const DockerError& e) {
        EXPECT_EQ(e.status_code(), 500);
        EXPECT_EQ(e.resource_id(), "abc123");
    }

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 4u);
    EXPECT_TRUE(request_is(requests[3], "DELETE /containers/abc123")) << requests[3];
}

TEST(Runner, ThrowingCreatedHookRemovesContainer) {
    CannedHttpServer server({ping_ok(), created("abc123"), removed()});
    testcontainers::DockerClient client{server.host()};

    ContainerRequest request = busybox_request();
    request.created_hooks.emplace_back([](testcontainers::DockerClient&, const std::string&) {
        throw std::runtime_error("hook boom");
    });

    EXPECT_THROW(Runner::run(client, request), std::runtime_error);

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 3u);
    EXPECT_TRUE(request_is(requests[2], "DELETE /containers/abc123")) << requests[2];
}

TEST(Runner, RetryCreatesFreshContainer) {
    CannedHttpServer server({
        ping_ok(),
        created("one"),
        server_error("first attempt fails"),
        removed(), // attempt 1
        created("two"),
        started(), // attempt 2
        removed(), // drop
    });
    ContainerRequest request = busybox_request();
    request.startup_attempts = 2;

    {
        testcontainers::DockerClient client{server.host()};
        const Container c = Runner::run(client, request);
        EXPECT_EQ(c.id(), "two");
    }

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 7u);
    EXPECT_TRUE(request_is(requests[3], "DELETE /containers/one")) << requests[3];
    EXPECT_TRUE(request_is(requests[4], "POST /containers/create")) << requests[4];
    EXPECT_TRUE(request_is(requests[6], "DELETE /containers/two")) << requests[6];
}

TEST(Runner, RetryExhaustionRethrowsLast) {
    CannedHttpServer server({
        ping_ok(),
        created("one"),
        server_error("first", 500),
        removed(),
        created("two"),
        server_error("second", 409),
        removed(),
    });
    testcontainers::DockerClient client{server.host()};

    ContainerRequest request = busybox_request();
    request.startup_attempts = 2;

    try {
        Runner::run(client, request);
        FAIL() << "expected DockerError";
    } catch (const DockerError& e) {
        // The LAST attempt's error is the one that propagates.
        EXPECT_EQ(e.status_code(), 409);
        EXPECT_EQ(e.resource_id(), "two");
    }

    EXPECT_EQ(server.requests().size(), 7u);
}

TEST(Runner, PullPolicyAlwaysPullsBeforeCreate) {
    CannedHttpServer server({
        ping_ok(),
        http_response(200, "OK", "{\"status\":\"Pulling from library/busybox\"}\n"),
        created("abc123"),
        started(),
        removed(),
    });
    ContainerRequest request = busybox_request();
    request.pull_policy = testcontainers::ImagePullPolicy::Always;
    // Explicit (empty) credentials keep the pull path off this host's Docker
    // config / credential helpers — the unit test must not shell out.
    request.registry_auth = testcontainers::RegistryAuth{};

    {
        testcontainers::DockerClient client{server.host()};
        const Container c = Runner::run(client, request);
        EXPECT_EQ(c.id(), "abc123");
    }

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 5u);
    EXPECT_TRUE(request_is(requests[1], "POST /images/create")) << requests[1];
    EXPECT_TRUE(request_is(requests[2], "POST /containers/create")) << requests[2];
}
