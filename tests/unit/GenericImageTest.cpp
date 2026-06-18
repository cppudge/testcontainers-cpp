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
//   GenericImage.ConfigBuildersReflectGetters - entrypoint/working-dir/user/privileged/mount builders record into the matching getters.
//   GenericImage.ConfigDefaults - a freshly constructed image has no entrypoint/working-dir/user/mounts and is not privileged.
//   GenericImage.NetworkDefaults - a freshly constructed image has no network or container name set.
//   GenericImage.NetworkBuildersReflectGetters - with_network and with_container_name record into the matching getters.
//   GenericImage.ConfigChainsOnRvalue - the new config builders chain on a temporary rvalue.
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

TEST(GenericImage, ConfigDefaults) {
    const GenericImage img("alpine", "3.20");
    EXPECT_TRUE(img.entrypoint().empty());
    EXPECT_FALSE(img.working_dir().has_value());
    EXPECT_FALSE(img.user().has_value());
    EXPECT_FALSE(img.privileged());
    EXPECT_TRUE(img.mounts().empty());
}

TEST(GenericImage, NetworkDefaults) {
    const GenericImage img("alpine", "3.20");
    EXPECT_FALSE(img.network().has_value());
    EXPECT_FALSE(img.container_name().has_value());
}

TEST(GenericImage, NetworkBuildersReflectGetters) {
    GenericImage img("redis", "7.2");
    img.with_network("my-net").with_container_name("redis-srv");

    ASSERT_TRUE(img.network().has_value());
    EXPECT_EQ(*img.network(), "my-net");
    ASSERT_TRUE(img.container_name().has_value());
    EXPECT_EQ(*img.container_name(), "redis-srv");
}

TEST(GenericImage, ConfigBuildersReflectGetters) {
    GenericImage img("alpine", "3.20");
    img.with_entrypoint({"echo"})
        .with_working_dir("/tmp")
        .with_user("1000:1000")
        .with_privileged()
        .with_mount(Mount::tmpfs("/cache").with_tmpfs_size(1024))
        .with_mount(Mount::bind("/host", "/data").read_only());

    EXPECT_EQ(img.entrypoint(), (std::vector<std::string>{"echo"}));
    ASSERT_TRUE(img.working_dir().has_value());
    EXPECT_EQ(*img.working_dir(), "/tmp");
    ASSERT_TRUE(img.user().has_value());
    EXPECT_EQ(*img.user(), "1000:1000");
    EXPECT_TRUE(img.privileged());

    ASSERT_EQ(img.mounts().size(), 2u);
    EXPECT_EQ(img.mounts()[0].type(), MountType::Tmpfs);
    EXPECT_EQ(img.mounts()[0].target(), "/cache");
    ASSERT_TRUE(img.mounts()[0].tmpfs_size().has_value());
    EXPECT_EQ(*img.mounts()[0].tmpfs_size(), 1024);
    EXPECT_EQ(img.mounts()[1].type(), MountType::Bind);
    EXPECT_TRUE(img.mounts()[1].is_read_only());
}

TEST(GenericImage, ConfigChainsOnRvalue) {
    const GenericImage img = GenericImage("alpine", "3.20")
                                 .with_entrypoint({"echo"})
                                 .with_working_dir("/tmp")
                                 .with_user("root")
                                 .with_privileged(true)
                                 .with_mount(Mount::tmpfs("/cache"));
    EXPECT_EQ(img.entrypoint(), (std::vector<std::string>{"echo"}));
    EXPECT_EQ(*img.working_dir(), "/tmp");
    EXPECT_EQ(*img.user(), "root");
    EXPECT_TRUE(img.privileged());
    ASSERT_EQ(img.mounts().size(), 1u);
    EXPECT_EQ(img.mounts()[0].type(), MountType::Tmpfs);
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
