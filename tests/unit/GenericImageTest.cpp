#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include "testcontainers/GenericImage.hpp"

// Tests in this file:
//   GenericImage.DefaultsTagAndTimeout - a freshly constructed image defaults to the "latest" tag and a 60s startup timeout.
//   GenericImage.ExplicitTag - the two-argument constructor stores the given tag.
//   GenericImage.GettersReflectBuilders - each with_* builder records into the matching getter.
//   GenericImage.ChainsOnLvalue - with_* chains on a named lvalue and accumulates all settings.
//   GenericImage.ChainsOnRvalue - with_* chains on a temporary rvalue and accumulates all settings.
//   GenericImage.ReusableAfterWith - a named image survives a with_* call and reflects both early and later settings (no use-after-move).

using namespace testcontainers;

TEST(GenericImage, DefaultsTagAndTimeout) {
    const GenericImage img("redis");
    EXPECT_EQ(img.image(), "redis");
    EXPECT_EQ(img.tag(), "latest");
    EXPECT_EQ(img.startup_timeout(), std::chrono::seconds(60));
    EXPECT_TRUE(img.exposed_ports().empty());
    EXPECT_TRUE(img.env().empty());
    EXPECT_TRUE(img.cmd().empty());
    EXPECT_TRUE(img.labels().empty());
    EXPECT_TRUE(img.waits().empty());
}

TEST(GenericImage, ExplicitTag) {
    const GenericImage img("redis", "7.2");
    EXPECT_EQ(img.image(), "redis");
    EXPECT_EQ(img.tag(), "7.2");
}

TEST(GenericImage, GettersReflectBuilders) {
    GenericImage img("redis", "7.2");
    img.with_exposed_port(tcp(6379))
        .with_env("MODE", "standalone")
        .with_cmd({"redis-server", "--appendonly", "yes"})
        .with_label("owner", "tc")
        .with_wait(wait_for::stdout_message("Ready"))
        .with_startup_timeout(std::chrono::milliseconds(5000));

    ASSERT_EQ(img.exposed_ports().size(), 1u);
    EXPECT_EQ(img.exposed_ports()[0], tcp(6379));

    ASSERT_EQ(img.env().size(), 1u);
    EXPECT_EQ(img.env()[0].first, "MODE");
    EXPECT_EQ(img.env()[0].second, "standalone");

    EXPECT_EQ(img.cmd(), (std::vector<std::string>{"redis-server", "--appendonly", "yes"}));

    ASSERT_EQ(img.labels().size(), 1u);
    EXPECT_EQ(img.labels()[0].first, "owner");
    EXPECT_EQ(img.labels()[0].second, "tc");

    ASSERT_EQ(img.waits().size(), 1u);
    ASSERT_TRUE(std::holds_alternative<wait::LogMessage>(img.waits()[0]));
    EXPECT_EQ(std::get<wait::LogMessage>(img.waits()[0]).text, "Ready");

    EXPECT_EQ(img.startup_timeout(), std::chrono::milliseconds(5000));
}

TEST(GenericImage, ChainsOnLvalue) {
    GenericImage img("redis", "7.2");
    GenericImage& ref = img.with_exposed_port(tcp(6379)).with_env("A", "1");
    EXPECT_EQ(&ref, &img); // chaining returns the same object
    EXPECT_EQ(img.exposed_ports().size(), 1u);
    EXPECT_EQ(img.env().size(), 1u);
}

TEST(GenericImage, ChainsOnRvalue) {
    const GenericImage img = GenericImage("redis", "7.2")
                                 .with_exposed_port(tcp(6379))
                                 .with_env("A", "1")
                                 .with_wait(wait_for::log("Ready"));
    EXPECT_EQ(img.exposed_ports().size(), 1u);
    EXPECT_EQ(img.env().size(), 1u);
    EXPECT_EQ(img.waits().size(), 1u);
}

TEST(GenericImage, ReusableAfterWith) {
    // The key anti-use-after-move check: a named image stays valid across
    // separate with_* calls, and getters reflect every change.
    GenericImage img("redis", "7.2");
    img.with_exposed_port(tcp(6379)); // configure once
    EXPECT_EQ(img.exposed_ports().size(), 1u);

    img.with_env("MODE", "standalone"); // configure more
    EXPECT_EQ(img.env().size(), 1u);

    // Earlier setting is still present alongside the later one.
    ASSERT_EQ(img.exposed_ports().size(), 1u);
    EXPECT_EQ(img.exposed_ports()[0], tcp(6379));
    ASSERT_EQ(img.env().size(), 1u);
    EXPECT_EQ(img.env()[0].first, "MODE");

    img.with_exposed_port(tcp(6380)); // and again
    EXPECT_EQ(img.exposed_ports().size(), 2u);
}
