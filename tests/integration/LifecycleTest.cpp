#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <string>
#include <vector>

#include "testcontainers/Container.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/Lifecycle.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"
#include "WindowsEngine.hpp"

// Tests in this file (integration; require a Docker daemon — Linux mode for the
// Lifecycle suite, Windows mode for the WindowsLifecycle mirror):
//   Lifecycle.HooksFireInOrder - created/starting/started hooks fire once, in that order, each seeing the live container id.
//   Lifecycle.StoppingHookFiresOnStop - a stopping hook fires when the container is explicitly stopped.
//   Lifecycle.StartupRetriesOnFailure - with_startup_attempts(2) retries the whole create→start→wait on failure, creating a fresh container each attempt.
//   Lifecycle.KeepLeavesContainerRunning - keep() releases removal ownership: after the handle drops, the container is still running (verified and cleaned up via an adopted RemoveOnDrop handle).
//   WindowsLifecycle.HooksFireInOrder - the same hook ordering against a Windows daemon (the hooks are client-side, but each leg drives real Windows-engine create/start calls).
//   WindowsLifecycle.StartupRetriesOnFailure - startup attempts retry with a fresh Windows container per attempt.

using namespace testcontainers;

namespace {

constexpr const char* kImage = "alpine:3.20";

} // namespace

// Requires a reachable Linux Docker daemon; each test skips if none is available.
class Lifecycle : public ::testing::Test {
protected:
    void SetUp() override {
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

TEST_F(Lifecycle, HooksFireInOrder) {
    std::vector<std::string> order;
    std::string seen_id;

    GenericImage image = GenericImage::from_reference(kImage);
    image.with_cmd({"sleep", "30"})
        .with_created_hook([&](DockerClient&, const std::string& id) {
            EXPECT_FALSE(id.empty());
            seen_id = id;
            order.emplace_back("created");
        })
        .with_starting_hook([&](DockerClient&, const std::string& id) {
            EXPECT_FALSE(id.empty());
            order.emplace_back("starting");
        })
        .with_started_hook([&](DockerClient&, const std::string& id) {
            EXPECT_FALSE(id.empty());
            order.emplace_back("started");
        });

    Container c = image.start();

    EXPECT_EQ(order, (std::vector<std::string>{"created", "starting", "started"}));
    // Each hook saw the same id as the live handle.
    EXPECT_EQ(seen_id, c.id());
}

TEST_F(Lifecycle, StoppingHookFiresOnStop) {
    bool stopping_fired = false;

    GenericImage image = GenericImage::from_reference(kImage);
    image.with_cmd({"sleep", "30"}).with_stopping_hook([&](DockerClient&, const std::string&) {
        stopping_fired = true;
    });

    Container c = image.start();
    EXPECT_FALSE(stopping_fired); // not fired until teardown

    c.stop();
    EXPECT_TRUE(stopping_fired);
}

TEST_F(Lifecycle, StartupRetriesOnFailure) {
    int created_count = 0;

    GenericImage image = GenericImage::from_reference(kImage);
    image
        .with_cmd({"sleep", "30"})
        // An impossible wait forces every attempt to fail (the container never
        // logs this), so start() exhausts its attempts and throws.
        .with_wait(wait_for::log("THIS_NEVER_APPEARS"))
        .with_startup_timeout(std::chrono::seconds(2))
        .with_startup_attempts(2)
        .with_created_hook([&](DockerClient&, const std::string&) { ++created_count; });

    EXPECT_THROW(image.start(), std::exception);
    // Exactly two attempts, each creating a fresh container (the created hook
    // runs once per create).
    EXPECT_EQ(created_count, 2);
}

TEST_F(Lifecycle, KeepLeavesContainerRunning) {
    // keep() releases removal ownership: the handle's drop leaves the container
    // running, exactly like a with_reuse handle would.
    std::string id;
    {
        Container c = GenericImage::from_reference(kImage).with_cmd({"sleep", "60"}).start();
        id = c.id();
        EXPECT_FALSE(c.is_persistent());
        c.keep();
        EXPECT_TRUE(c.is_persistent());
    } // drop: must NOT remove the kept container

    // The container survived the drop; adopt it with RemoveOnDrop so one handle
    // both proves it is still running and cleans it up.
    Container adopted =
        Container::adopt(DockerClient::from_environment(), id, AdoptOwnership::RemoveOnDrop);
    EXPECT_TRUE(adopted.is_running());
}

// The Windows mirror: the hook plumbing is client-side, but each leg drives a
// real Windows-engine create/start/wait round-trip.
class WindowsLifecycle : public tcit::WindowsEngineTest {};

TEST_F(WindowsLifecycle, HooksFireInOrder) {
    std::vector<std::string> order;
    std::string seen_id;

    GenericImage image = nanoserver();
    image.with_cmd(keep_alive_cmd())
        .with_created_hook([&](DockerClient&, const std::string& id) {
            EXPECT_FALSE(id.empty());
            seen_id = id;
            order.emplace_back("created");
        })
        .with_starting_hook([&](DockerClient&, const std::string& id) {
            EXPECT_FALSE(id.empty());
            order.emplace_back("starting");
        })
        .with_started_hook([&](DockerClient&, const std::string& id) {
            EXPECT_FALSE(id.empty());
            order.emplace_back("started");
        });

    Container c = image.start();

    EXPECT_EQ(order, (std::vector<std::string>{"created", "starting", "started"}));
    EXPECT_EQ(seen_id, c.id());
}

TEST_F(WindowsLifecycle, StartupRetriesOnFailure) {
    int created_count = 0;

    GenericImage image = nanoserver();
    image.with_cmd(keep_alive_cmd())
        .with_wait(wait_for::log("THIS_NEVER_APPEARS"))
        .with_startup_timeout(std::chrono::seconds(2))
        .with_startup_attempts(2)
        .with_created_hook([&](DockerClient&, const std::string&) { ++created_count; });

    EXPECT_THROW(image.start(), std::exception);
    // Exactly two attempts, each creating a fresh Windows container.
    EXPECT_EQ(created_count, 2);
}
