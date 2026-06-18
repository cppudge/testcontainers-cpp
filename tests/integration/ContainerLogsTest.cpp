#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <string>
#include <string_view>
#include <thread>

#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon):
//   DockerLogs.FetchesStdoutAndStderr - a container's stdout and stderr are fetched and demultiplexed into separate streams without cross-contamination.
//   DockerLogs.FollowStreamsUntilExit - follow_logs streams a short-lived container's lines in order and returns when the container exits.
//   DockerLogs.FollowStopsEarlyWhenConsumerReturnsFalse - follow_logs returns promptly once the consumer returns false on a long-running container, proving frames stream incrementally (not batched to EOF).

using namespace testcontainers;

namespace {

constexpr const char* kImage = "alpine:3.20";

// Best-effort force-remove on scope exit so tests never leak containers.
struct RemoveGuard {
    DockerClient& client;
    std::string id;
    ~RemoveGuard() {
        try {
            if (!id.empty()) {
                client.remove_container(id, /*force*/ true, /*remove_volumes*/ true);
            }
        } catch (...) {
        }
    }
};

} // namespace

// Requires a reachable Docker daemon; each test is skipped if none is available.
class DockerLogs : public ::testing::Test {
protected:
    DockerClient client = DockerClient::from_environment();

    void SetUp() override {
        if (auto why = tcit::linux_engine_unavailable()) {
            GTEST_SKIP() << *why;
        }
    }

    // Poll inspect until the container is no longer running (or we give up).
    void wait_until_exited(const std::string& id) {
        for (int i = 0; i < 100; ++i) {
            if (!client.inspect_container(id).running) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
};

TEST_F(DockerLogs, FetchesStdoutAndStderr) {
    client.pull_image(kImage);

    CreateContainerSpec spec;
    spec.image = kImage;
    spec.cmd = {"sh", "-c", "echo hello-stdout; echo hello-stderr 1>&2"};

    const std::string id = client.create_container(spec);
    ASSERT_FALSE(id.empty());
    RemoveGuard guard{client, id};

    client.start_container(id);
    wait_until_exited(id);

    const ContainerLogs logs = client.logs(id);
    EXPECT_NE(logs.stdout_data.find("hello-stdout"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_NE(logs.stderr_data.find("hello-stderr"), std::string::npos)
        << "stderr was: " << logs.stderr_data;
    // Streams must stay separated.
    EXPECT_EQ(logs.stdout_data.find("hello-stderr"), std::string::npos);
    EXPECT_EQ(logs.stderr_data.find("hello-stdout"), std::string::npos);
}

TEST_F(DockerLogs, FollowStreamsUntilExit) {
    client.pull_image(kImage);

    CreateContainerSpec spec;
    spec.image = kImage;
    // Prints three lines spaced ~0.3s apart, then exits (total loop ~1s).
    spec.cmd = {"sh", "-c", "for i in 1 2 3; do echo line-$i; sleep 0.3; done"};

    const std::string id = client.create_container(spec);
    ASSERT_FALSE(id.empty());
    RemoveGuard guard{client, id};

    client.start_container(id);

    // follow_logs blocks until the container's stream ends (it exits). Bound the
    // wall-clock so a hang fails loudly rather than stalling the suite.
    std::string collected;
    const auto started = std::chrono::steady_clock::now();
    client.follow_logs(id, LogOptions{}, [&](LogSource source, std::string_view data) {
        if (source == LogSource::Stdout) {
            collected.append(data.data(), data.size());
        }
        return true; // keep receiving until the stream ends
    });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_LT(elapsed, std::chrono::seconds(30)) << "follow_logs did not return promptly";

    // All three lines must have streamed through, in order.
    const auto p1 = collected.find("line-1");
    const auto p2 = collected.find("line-2");
    const auto p3 = collected.find("line-3");
    ASSERT_NE(p1, std::string::npos) << "stdout was: " << collected;
    ASSERT_NE(p2, std::string::npos) << "stdout was: " << collected;
    ASSERT_NE(p3, std::string::npos) << "stdout was: " << collected;
    EXPECT_LT(p1, p2);
    EXPECT_LT(p2, p3);
}

TEST_F(DockerLogs, FollowStopsEarlyWhenConsumerReturnsFalse) {
    client.pull_image(kImage);

    CreateContainerSpec spec;
    spec.image = kImage;
    // A long-running emitter (up to ~400s). The consumer stops after line-1, so
    // this also proves frames are delivered INCREMENTALLY: if follow_logs batched
    // the body (read instead of read_some) it would block until the 8 KiB buffer
    // filled (~hundreds of seconds), and the elapsed-time bound below would fail.
    spec.cmd = {"sh", "-c", "for i in $(seq 1 2000); do echo line-$i; sleep 0.2; done"};

    const std::string id = client.create_container(spec);
    ASSERT_FALSE(id.empty());
    RemoveGuard guard{client, id};

    client.start_container(id);

    // The consumer stops as soon as it has seen line-1; follow_logs must return
    // promptly afterwards (closing the connection stops the stream).
    std::string collected;
    const auto started = std::chrono::steady_clock::now();
    client.follow_logs(id, LogOptions{}, [&](LogSource source, std::string_view data) {
        if (source == LogSource::Stdout) {
            collected.append(data.data(), data.size());
        }
        return collected.find("line-1") == std::string::npos; // stop once line-1 arrived
    });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    // Generous bound: read_some delivers line-1 in well under a second; the old
    // batching read would take hundreds of seconds (one full 8 KiB buffer).
    EXPECT_LT(elapsed, std::chrono::seconds(10)) << "follow_logs did not stop promptly";

    // line-1 must be present; later lines may or may not be, depending on timing.
    EXPECT_NE(collected.find("line-1"), std::string::npos) << "stdout was: " << collected;
}
