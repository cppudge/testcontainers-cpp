#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <random>
#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon):
//   Reuse.ReuseAdoptsRunningContainer - with reuse enabled, two identical with_reuse(true) starts adopt the same running container (same id).
//   Reuse.ReuseDisabledCreatesFresh - with reuse NOT enabled, two with_reuse(true) starts create different containers (degrades to normal).

using namespace testcontainers;

namespace {

void set_env(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value ? value : ""); // empty value removes it
#else
    if (value) {
        setenv(key, value, 1);
    } else {
        unsetenv(key);
    }
#endif
}

// A unique marker so each run's reuse config can't collide with a stale
// container left over from an earlier run.
std::string unique_marker() {
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    static constexpr char hex[] = "0123456789abcdef";
    std::string out = "reuse-it-";
    for (int i = 0; i < 16; ++i) {
        out.push_back(hex[dist(rd)]);
    }
    return out;
}

} // namespace

// Requires a reachable Linux Docker daemon; skipped otherwise.
class Reuse : public ::testing::Test {
protected:
    void SetUp() override {
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

TEST_F(Reuse, ReuseAdoptsRunningContainer) {
    // Enable reuse for this test only; restore the env var afterwards.
    const char* saved = std::getenv("TESTCONTAINERS_REUSE_ENABLE");
    const std::string previous = saved ? saved : "";
    const bool had_value = saved != nullptr;
    set_env("TESTCONTAINERS_REUSE_ENABLE", "true");

    const GenericImage img = GenericImage("alpine", "3.20")
                                 .with_env("TC_REUSE_MARKER", unique_marker())
                                 .with_cmd({"sh", "-c", "sleep 120"})
                                 .with_reuse(true);

    std::string adopted_id;
    try {
        const Container a = img.start();
        adopted_id = a.id();
        ASSERT_FALSE(adopted_id.empty());
        EXPECT_TRUE(a.is_persistent()); // reuse handles do not auto-remove

        // A second identical start must adopt the first (running) container.
        const Container b = img.start();
        EXPECT_EQ(a.id(), b.id()) << "second start did not reuse the first container";
        EXPECT_TRUE(b.is_persistent());
    } catch (...) {
        // Manual cleanup: reuse containers are persistent and NOT reaped, so the
        // non-removing handles won't remove it — we must do it ourselves.
        if (!adopted_id.empty()) {
            try {
                DockerClient::from_environment().remove_container(adopted_id, true, true);
            } catch (...) {
            }
        }
        set_env("TESTCONTAINERS_REUSE_ENABLE", had_value ? previous.c_str() : nullptr);
        throw;
    }

    // Manual cleanup (see above): the handles dropped without removing anything.
    ASSERT_FALSE(adopted_id.empty());
    EXPECT_NO_THROW(DockerClient::from_environment().remove_container(adopted_id, true, true));

    set_env("TESTCONTAINERS_REUSE_ENABLE", had_value ? previous.c_str() : nullptr);
}

TEST_F(Reuse, ReuseDisabledCreatesFresh) {
    // With reuse NOT enabled globally, with_reuse(true) degrades to a normal
    // (reaped, auto-removed) container: two starts make two different containers.
    const char* saved = std::getenv("TESTCONTAINERS_REUSE_ENABLE");
    const std::string previous = saved ? saved : "";
    const bool had_value = saved != nullptr;
    set_env("TESTCONTAINERS_REUSE_ENABLE", nullptr); // ensure disabled

    const GenericImage img = GenericImage("alpine", "3.20")
                                 .with_env("TC_REUSE_MARKER", unique_marker())
                                 .with_cmd({"sh", "-c", "sleep 30"})
                                 .with_reuse(true);

    // Both are normal handles that auto-remove on drop — no manual cleanup needed.
    const Container a = img.start();
    const Container b = img.start();
    EXPECT_NE(a.id(), b.id()) << "reuse should be inactive when not enabled globally";
    EXPECT_FALSE(a.is_persistent());
    EXPECT_FALSE(b.is_persistent());

    set_env("TESTCONTAINERS_REUSE_ENABLE", had_value ? previous.c_str() : nullptr);
}
