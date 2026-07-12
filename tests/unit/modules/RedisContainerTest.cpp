#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"
// The umbrella on purpose: this TU compile-checks testcontainers/modules.hpp.
#include "testcontainers/modules.hpp"

// Tests in this file (unit; no Docker daemon — the module's rendering rules
// via to_generic()):
//   RedisModuleConfig.DefaultRendersPinPortAndPingWait - the default config renders the pinned image, exposes 6379/tcp, installs the redis-cli ping command wait, and leaves cmd/env untouched (image defaults preserved).
//   RedisModuleConfig.PasswordOwnsCmdAndAuthEnv - with_password renders {"redis-server","--requirepass",pw} and appends REDISCLI_AUTH so in-container redis-cli (the probe, user execs) authenticates.
//   RedisModuleConfig.CommandArgsAccumulateAfterRequirepass - repeated with_command_args calls accumulate in order, placed after --requirepass.
//   RedisModuleConfig.ArgsAloneOwnCmdWithoutEnv - args without a password render {"redis-server", args...} and add no env key.
//   RedisModuleConfig.CustomizerRunsLastAndWins - a customizer sees the module-rendered builder, runs in registration order, and what it sets overrides the module's rendering.
//   RedisModuleConfig.WithImageRewritesReference - with_image swaps name and tag (bare name defaults to "latest") while the rest of the config survives.
//   RedisModuleConfig.RenderingIsIdempotent - to_generic() renders into a copy: repeated renders are equal and the config never accumulates state.
//   RedisModuleConfig.CommandArgSingleTwinAccumulates - with_command_arg (the single-arg twin) and with_command_args interleave into one ordered arg list.
//   RedisModuleConfig.PassThroughsLandOnTheImage - the env/label/network/alias/reuse/timeout/attempts pass-throughs reach the rendered builder.
//   RedisModuleConfig.ManagedAuthEnvConflictThrowsAtRender - REDISCLI_AUTH via with_env alongside a password throws at render (exec'd redis-cli reads the FIRST duplicate, so ordering could not make the module win); without a password the key is the user's.

using namespace testcontainers;
using modules::RedisContainer;

TEST(RedisModuleConfig, DefaultRendersPinPortAndPingWait) {
    const GenericImage generic = RedisContainer().to_generic();

    EXPECT_EQ(generic.image(), "redis");
    EXPECT_EQ(generic.tag(), "7.2");
    ASSERT_EQ(generic.exposed_ports().size(), 1u);
    EXPECT_EQ(generic.exposed_ports()[0], tcp(6379));
    ASSERT_EQ(generic.waits().size(), 1u);
    const auto* command = std::get_if<wait_for::Command>(&generic.waits()[0]);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->cmd, (std::vector<std::string>{"redis-cli", "ping"}));
    // Image defaults preserved: the module owns cmd/env only when told to.
    EXPECT_TRUE(generic.cmd().empty());
    EXPECT_TRUE(generic.env().empty());
}

TEST(RedisModuleConfig, PasswordOwnsCmdAndAuthEnv) {
    const GenericImage generic = RedisContainer().with_password("s3cr3t").to_generic();

    EXPECT_EQ(generic.cmd(), (std::vector<std::string>{"redis-server", "--requirepass", "s3cr3t"}));
    ASSERT_EQ(generic.env().size(), 1u);
    EXPECT_EQ(generic.env()[0].first, "REDISCLI_AUTH");
    EXPECT_EQ(generic.env()[0].second, "s3cr3t");
}

TEST(RedisModuleConfig, CommandArgsAccumulateAfterRequirepass) {
    RedisContainer cfg;
    cfg.with_password("pw").with_command_args({"--maxmemory", "64mb"});
    cfg.with_command_args({"--appendonly", "no"});

    EXPECT_EQ(cfg.to_generic().cmd(),
              (std::vector<std::string>{"redis-server", "--requirepass", "pw", "--maxmemory",
                                        "64mb", "--appendonly", "no"}));
    EXPECT_EQ(cfg.command_args(),
              (std::vector<std::string>{"--maxmemory", "64mb", "--appendonly", "no"}));
}

TEST(RedisModuleConfig, ArgsAloneOwnCmdWithoutEnv) {
    const GenericImage generic =
        RedisContainer().with_command_args({"--maxmemory", "64mb"}).to_generic();

    EXPECT_EQ(generic.cmd(), (std::vector<std::string>{"redis-server", "--maxmemory", "64mb"}));
    EXPECT_TRUE(generic.env().empty());
}

TEST(RedisModuleConfig, CustomizerRunsLastAndWins) {
    RedisContainer cfg;
    cfg.with_password("pw")
        .with_customizer([](GenericImage& generic) {
            // Runs after the module's rendering, so it sees the rendered cmd.
            ASSERT_FALSE(generic.cmd().empty());
            EXPECT_EQ(generic.cmd()[0], "redis-server");
            generic.with_label("team", "cache");
        })
        .with_customizer(
            [](GenericImage& generic) { generic.with_cmd({"redis-server", "/etc/redis.conf"}); });

    const GenericImage generic = cfg.to_generic();
    // The later customizer's cmd wins over the module's rendering.
    EXPECT_EQ(generic.cmd(), (std::vector<std::string>{"redis-server", "/etc/redis.conf"}));
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
    // The module's env key is untouched by the cmd override.
    ASSERT_EQ(generic.env().size(), 1u);
    EXPECT_EQ(generic.env()[0].first, "REDISCLI_AUTH");
}

TEST(RedisModuleConfig, WithImageRewritesReference) {
    RedisContainer cfg;
    cfg.with_password("pw").with_image("mirror.example/redis:7.4");

    const GenericImage generic = cfg.to_generic();
    EXPECT_EQ(generic.image(), "mirror.example/redis");
    EXPECT_EQ(generic.tag(), "7.4");
    ASSERT_EQ(generic.cmd().size(), 3u); // the rest of the config survives the swap
    EXPECT_EQ(generic.cmd()[1], "--requirepass");

    cfg.with_image("valkey/valkey"); // bare name defaults to "latest"
    EXPECT_EQ(cfg.to_generic().image(), "valkey/valkey");
    EXPECT_EQ(cfg.to_generic().tag(), "latest");
}

TEST(RedisModuleConfig, RenderingIsIdempotent) {
    RedisContainer cfg;
    cfg.with_password("pw").with_command_args({"--maxmemory", "64mb"});

    const GenericImage first = cfg.to_generic();
    const GenericImage second = cfg.to_generic();
    EXPECT_EQ(first.cmd(), second.cmd());
    EXPECT_EQ(first.env(), second.env());
    ASSERT_EQ(second.waits().size(), 1u); // the ping wait is baked once, never re-appended
}

TEST(RedisModuleConfig, CommandArgSingleTwinAccumulates) {
    RedisContainer cfg;
    cfg.with_command_arg("--maxmemory").with_command_arg("64mb");
    cfg.with_command_args({"--appendonly", "no"});

    EXPECT_EQ(cfg.to_generic().cmd(), (std::vector<std::string>{"redis-server", "--maxmemory",
                                                                "64mb", "--appendonly", "no"}));
}

TEST(RedisModuleConfig, PassThroughsLandOnTheImage) {
    RedisContainer cfg;
    cfg.with_env("TZ", "UTC")
        .with_label("team", "cache")
        .with_network("net-a")
        .with_network_alias("cache")
        .with_reuse()
        .with_startup_timeout(std::chrono::seconds(90))
        .with_startup_attempts(2);

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.env().size(), 1u);
    EXPECT_EQ(generic.env()[0].first, "TZ");
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
    EXPECT_EQ(generic.network().value_or(""), "net-a");
    ASSERT_EQ(generic.network_aliases().size(), 1u);
    EXPECT_EQ(generic.network_aliases()[0], "cache");
    EXPECT_TRUE(generic.reuse());
    EXPECT_EQ(generic.startup_timeout(), std::chrono::seconds(90));
    EXPECT_EQ(generic.startup_attempts(), 2);
}

TEST(RedisModuleConfig, ManagedAuthEnvConflictThrowsAtRender) {
    // With a password the module owns REDISCLI_AUTH: exec'd redis-cli reads
    // the FIRST duplicate of a key, so ordering could not make the module's
    // entry win over a raw one.
    EXPECT_THROW(RedisContainer().with_password("pw").with_env("REDISCLI_AUTH", "x").to_generic(),
                 Error);
    // Without a password the key is the user's (custom auth setups).
    const GenericImage generic = RedisContainer().with_env("REDISCLI_AUTH", "x").to_generic();
    ASSERT_EQ(generic.env().size(), 1u);
    EXPECT_EQ(generic.env()[0].first, "REDISCLI_AUTH");
}
