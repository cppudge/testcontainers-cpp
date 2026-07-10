#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "CannedHttpServer.hpp"
#include "Runner.hpp"
#include "TestEnv.hpp"
#include "TestSupport.hpp"
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
//   Runner.KeepFalseRearmsRemovalOnDrop - keep(false) undoes keep(): the handle is auto-removing again and its drop DELETEs like the happy path.
//   Runner.HooksFireInOrderAroundCopy - created/starting/started/stopping hooks fire in order with the container id, with the copy-to PUT between create and start.
//   Runner.FailedStartRemovesPartialContainer - a 500 on start propagates as DockerError(500) after the partial container is force-removed.
//   Runner.ThrowingCreatedHookRemovesContainer - an exception from a created hook aborts the run and still removes the partial container.
//   Runner.RetryCreatesFreshContainer - with startup_attempts=2 a failed first attempt is removed and a brand-new container is created and returned.
//   Runner.RetryExhaustionRethrowsLast - when every attempt fails, the LAST attempt's error propagates (each partial container removed).
//   Runner.PullPolicyAlwaysPullsBeforeCreate - ImagePullPolicy::Always issues POST /images/create before the container create.
//   Runner.WaitTimeoutRemovesContainerAndConsumesAttempts - a readiness timeout surfaces as StartupTimeoutError, force-removes the partial container, and consumes a startup attempt (it IS retried).
//   Runner.ReuseAdoptsRunningMatch - with reuse enabled, a running hash-match is adopted (no create, not retried) and the handle is persistent (no DELETE on drop).
//   Runner.ReuseCreateBodyCarriesHashNotSessionLabel - with reuse enabled and no match, the fresh container's create body carries the reuse-hash label and NEVER the session-id label (Ryuk must not reap it); the handle is persistent.
//   Runner.ReuseHashTracksHostFileFreshness - a host-file copy-to source hashes size+mtime: the hash is stable while the file is unchanged, and changes on an in-place edit and on an mtime-only bump.
//   Runner.ReuseHashCoversDirectoryTree - a host-dir source hashes the sorted walked tree: stable across runs, changed by editing a nested file or adding an empty directory.

namespace {

using tctest::ScopedEnv;
using tcunit::CannedHttpServer;
using tcunit::created;
using tcunit::http_response;
using tcunit::http_response_no_body;
using tcunit::ping_ok;
using tcunit::request_is;

using testcontainers::Container;
using testcontainers::ContainerRequest;
using testcontainers::CopyToContainer;
using testcontainers::DockerError;
using testcontainers::detail::Runner;

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
    // The spare removed() entry is a tripwire: if keep() regressed and drop
    // DID send a DELETE, that connection gets accepted and recorded (failing
    // the count below) instead of hanging unaccepted until the io timeout.
    CannedHttpServer server({ping_ok(), created("abc123"), started(), removed()});
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

TEST(Runner, KeepFalseRearmsRemovalOnDrop) {
    // keep(false) undoes keep(): removal ownership is re-armed, so the drop
    // issues the DELETE exactly like the happy path.
    CannedHttpServer server({ping_ok(), created("abc123"), started(), removed()});
    {
        testcontainers::DockerClient client{server.host()};
        Container c = Runner::run(client, busybox_request());
        c.keep();
        EXPECT_TRUE(c.is_persistent());
        c.keep(false);
        EXPECT_FALSE(c.is_persistent());
    } // drop -> DELETE again

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 4u);
    EXPECT_TRUE(request_is(requests[3], "DELETE /containers/abc123")) << requests[3];
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
    // The spare removed() entry is a tripwire (the KeepSkipsRemovalOnDrop
    // pattern): if the persistent handle regressed and DID send a DELETE on
    // drop, that connection gets accepted and recorded — failing the count
    // below — instead of hanging unaccepted until the io timeout.
    CannedHttpServer server({ping_ok(),
                             http_response(200, "OK", R"([{"Id":"reused1","State":"running"}])"),
                             removed()});

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
        removed(), // tripwire: an unexpected drop-time DELETE fails the count
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

namespace {

/// The 16-hex reuse-hash label value inside a recorded create request.
std::string reuse_hash_of(const std::string& create_request) {
    const std::string key = R"("org.testcontainers.reuse.hash":")";
    const std::size_t pos = create_request.find(key);
    EXPECT_NE(pos, std::string::npos) << create_request;
    return pos == std::string::npos ? std::string{} : create_request.substr(pos + key.size(), 16);
}

/// Run one reuse-enabled request (no canned match -> fresh create) against a
/// throwaway canned daemon and return the create body's reuse hash.
std::string reuse_hash_for(const ContainerRequest& request) {
    CannedHttpServer server({
        ping_ok(),
        http_response(200, "OK", "[]"), // no running match
        created("fresh1"),
        http_response(200, "OK", "{}"), // PUT /containers/fresh1/archive (copy-to)
        started(),
        removed(), // tripwire; a persistent drop sends nothing
    });
    testcontainers::DockerClient client{server.host()};
    {
        const Container c = Runner::run(client, request);
    }
    const auto requests = server.requests();
    EXPECT_EQ(requests.size(), 5u) << "expected ping, list, create, copy PUT, start";
    return requests.size() > 2 ? reuse_hash_of(requests[2]) : std::string{};
}

/// A self-cleaning temp directory for fixture files.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("tc_runner_reuse_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(dir_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    const std::filesystem::path& path() const { return dir_; }

private:
    std::filesystem::path dir_;
};

void write_file(const std::filesystem::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

} // namespace

TEST(Runner, ReuseHashTracksHostFileFreshness) {
    const ScopedEnv enable("TESTCONTAINERS_REUSE_ENABLE", "1");
    const TempDir tmp;
    const std::filesystem::path file = tmp.path() / "fixture.txt";
    write_file(file, "v1");

    ContainerRequest request = busybox_request();
    request.reuse = true;
    request.copy_to_sources = {CopyToContainer::host_file(file, "/data/fixture.txt")};

    // Unchanged file -> the same hash on every start (adoption works).
    const std::string first = reuse_hash_for(request);
    EXPECT_EQ(reuse_hash_for(request), first);

    // A size-changing edit in place -> a different hash (no stale adoption).
    write_file(file, "v2-now-longer");
    const std::string edited = reuse_hash_for(request);
    EXPECT_NE(edited, first);

    // An mtime-only change (same size) is also freshness: bump mtime explicitly.
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(file, ec);
    ASSERT_FALSE(ec);
    std::filesystem::last_write_time(file, mtime + std::chrono::hours(1), ec);
    ASSERT_FALSE(ec);
    EXPECT_NE(reuse_hash_for(request), edited);
}

TEST(Runner, ReuseHashCoversDirectoryTree) {
    const ScopedEnv enable("TESTCONTAINERS_REUSE_ENABLE", "1");
    const TempDir tmp;
    std::filesystem::create_directories(tmp.path() / "sub");
    write_file(tmp.path() / "a.txt", "alpha");
    write_file(tmp.path() / "sub" / "b.txt", "beta");

    ContainerRequest request = busybox_request();
    request.reuse = true;
    request.copy_to_sources = {CopyToContainer::host_dir(tmp.path(), "/data")};

    // Stable across runs (the walk is sorted, so iteration order cannot leak in).
    const std::string first = reuse_hash_for(request);
    EXPECT_EQ(reuse_hash_for(request), first);

    // Editing a nested file changes the tree's hash.
    write_file(tmp.path() / "sub" / "b.txt", "beta-changed");
    const std::string edited = reuse_hash_for(request);
    EXPECT_NE(edited, first);

    // So does adding an empty directory (it is copied, so it is identity).
    std::filesystem::create_directories(tmp.path() / "sub2");
    EXPECT_NE(reuse_hash_for(request), edited);
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
