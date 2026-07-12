#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/modules/NATS.hpp"

// Tests in this file (unit; no Docker daemon — the module's rendering rules
// via to_generic()):
//   NATSModuleConfig.DefaultRendersPinPortsWaitsAndMonitoringCmd - the default config renders the pinned image, exposes 4222+8222/tcp, installs the ordered log-then-/healthz waits, always owns cmd ({"-m","8222"}), and sets no env.
//   NATSModuleConfig.CredentialsAndJetstreamRenderInOrder - with_username/with_password/with_jetstream render {"-m","8222","--user",u,"--pass",p,"-js"} and the image getters report them.
//   NATSModuleConfig.HalfACredentialPairThrowsAtRender - a username without a password (and vice versa) throws at render, before any daemon contact.
//   NATSModuleConfig.CommandArgsAccumulateAfterManagedFlags - with_command_args/with_command_arg interleave into one ordered list appended after the managed flags.
//   NATSModuleConfig.ManagedFlagInArgsThrowsAtRender - every managed spelling (-/-- and =-forms of user/pass/js/jetstream/m/http_port/p/port/a/addr/net/c/config) throws at render; unmanaged flags and plain values pass.
//   NATSModuleConfig.CustomizerRunsLastAndWins - a customizer sees the module-rendered builder, runs in registration order, and what it sets overrides the module's rendering.
//   NATSModuleConfig.WithImageRewritesReference - with_image swaps name and tag (bare name defaults to "latest") while the rest of the config survives.
//   NATSModuleConfig.RenderingIsIdempotent - to_generic() renders into a copy: repeated renders are equal and the config never accumulates state.
//   NATSModuleConfig.PassThroughsLandOnTheImage - the env/label/network/alias/reuse/timeout/attempts pass-throughs reach the rendered builder.

using namespace testcontainers;
using modules::NATSImage;

TEST(NATSModuleConfig, DefaultRendersPinPortsWaitsAndMonitoringCmd) {
    const GenericImage generic = NATSImage().to_generic();

    EXPECT_EQ(generic.image(), "nats");
    EXPECT_EQ(generic.tag(), "2.12");
    ASSERT_EQ(generic.exposed_ports().size(), 2u);
    EXPECT_EQ(generic.exposed_ports()[0], tcp(4222));
    EXPECT_EQ(generic.exposed_ports()[1], tcp(8222));

    // Ordered: the log wait proves the process, the HTTP wait proves the
    // published port end to end.
    ASSERT_EQ(generic.waits().size(), 2u);
    const auto* log = std::get_if<wait_for::LogMessage>(&generic.waits()[0]);
    ASSERT_NE(log, nullptr);
    EXPECT_EQ(log->text, "Server is ready");
    EXPECT_EQ(log->source, wait_for::LogMessage::Source::Either);
    const auto* http = std::get_if<wait_for::Http>(&generic.waits()[1]);
    ASSERT_NE(http, nullptr);
    EXPECT_EQ(http->path, "/healthz");
    EXPECT_EQ(http->port, tcp(8222));
    EXPECT_EQ(http->expected_status, 200);

    // The module always owns the command: monitoring must be restated once
    // the image's default config file is dropped.
    EXPECT_EQ(generic.cmd(), (std::vector<std::string>{"-m", "8222"}));
    EXPECT_TRUE(generic.env().empty());
}

TEST(NATSModuleConfig, CredentialsAndJetstreamRenderInOrder) {
    NATSImage cfg;
    cfg.with_username("app").with_password("s3cr3t").with_jetstream();

    EXPECT_EQ(cfg.to_generic().cmd(),
              (std::vector<std::string>{"-m", "8222", "--user", "app", "--pass", "s3cr3t", "-js"}));
    EXPECT_EQ(cfg.username(), "app");
    EXPECT_EQ(cfg.password(), "s3cr3t");
    EXPECT_TRUE(cfg.jetstream());

    cfg.with_jetstream(false);
    EXPECT_EQ(cfg.to_generic().cmd(),
              (std::vector<std::string>{"-m", "8222", "--user", "app", "--pass", "s3cr3t"}));
}

TEST(NATSModuleConfig, HalfACredentialPairThrowsAtRender) {
    EXPECT_THROW(NATSImage().with_username("app").to_generic(), Error);
    EXPECT_THROW(NATSImage().with_password("s3cr3t").to_generic(), Error);
    // The full pair renders fine.
    EXPECT_NO_THROW(NATSImage().with_username("app").with_password("s3cr3t").to_generic());
}

TEST(NATSModuleConfig, CommandArgsAccumulateAfterManagedFlags) {
    NATSImage cfg;
    cfg.with_jetstream().with_command_arg("--name").with_command_arg("orders-bus");
    cfg.with_command_args({"--auth", "tok"});

    EXPECT_EQ(cfg.to_generic().cmd(), (std::vector<std::string>{"-m", "8222", "-js", "--name",
                                                                "orders-bus", "--auth", "tok"}));
    EXPECT_EQ(cfg.command_args(),
              (std::vector<std::string>{"--name", "orders-bus", "--auth", "tok"}));
}

TEST(NATSModuleConfig, ManagedFlagInArgsThrowsAtRender) {
    // The server keeps the LAST duplicate of a flag, so ordering could not
    // make the module's copy win — render throws on every managed spelling.
    const std::vector<std::string> managed{
        "--user",       "-user",
        "--user=app",   "--pass",
        "-pass=x",      "-js",
        "--js",         "--jetstream",
        "-jetstream",   "-m",
        "--m=0",        "--http_port",
        "-http_port=0", "-p",
        "--port",       "--port=5222",
        "-a",           "--addr",
        "--net",        "-c",
        "--config",     "--config=/etc/nats.conf",
    };
    for (const std::string& flag : managed) {
        EXPECT_THROW(NATSImage().with_command_arg(flag).to_generic(), Error) << flag;
    }

    // Unmanaged flags and plain values (even dash-less ones matching managed
    // names) pass through.
    EXPECT_NO_THROW(NATSImage().with_command_args({"--name", "orders-bus"}).to_generic());
    EXPECT_NO_THROW(NATSImage().with_command_args({"--auth", "user"}).to_generic());
    EXPECT_NO_THROW(NATSImage().with_command_arg("-DV").to_generic());
}

TEST(NATSModuleConfig, CustomizerRunsLastAndWins) {
    NATSImage cfg;
    cfg.with_jetstream()
        .with_customizer([](GenericImage& generic) {
            // Runs after the module's rendering, so it sees the rendered cmd.
            ASSERT_FALSE(generic.cmd().empty());
            EXPECT_EQ(generic.cmd()[0], "-m");
            generic.with_label("team", "messaging");
        })
        .with_customizer(
            [](GenericImage& generic) { generic.with_cmd({"-c", "/etc/nats/nats.conf"}); });

    const GenericImage generic = cfg.to_generic();
    // The later customizer's cmd wins over the module's rendering.
    EXPECT_EQ(generic.cmd(), (std::vector<std::string>{"-c", "/etc/nats/nats.conf"}));
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
}

TEST(NATSModuleConfig, WithImageRewritesReference) {
    NATSImage cfg;
    cfg.with_jetstream().with_image("mirror.example/nats:2.14-alpine");

    const GenericImage generic = cfg.to_generic();
    EXPECT_EQ(generic.image(), "mirror.example/nats");
    EXPECT_EQ(generic.tag(), "2.14-alpine");
    // The rest of the config survives the swap.
    EXPECT_EQ(generic.cmd(), (std::vector<std::string>{"-m", "8222", "-js"}));

    cfg.with_image("nats"); // bare name defaults to "latest"
    EXPECT_EQ(cfg.to_generic().image(), "nats");
    EXPECT_EQ(cfg.to_generic().tag(), "latest");
}

TEST(NATSModuleConfig, RenderingIsIdempotent) {
    NATSImage cfg;
    cfg.with_username("app").with_password("pw").with_command_args({"--name", "orders-bus"});

    const GenericImage first = cfg.to_generic();
    const GenericImage second = cfg.to_generic();
    EXPECT_EQ(first.cmd(), second.cmd());
    EXPECT_EQ(first.env(), second.env());
    ASSERT_EQ(second.waits().size(), 2u); // the wait pair is baked once, never re-appended
}

TEST(NATSModuleConfig, PassThroughsLandOnTheImage) {
    NATSImage cfg;
    cfg.with_env("TZ", "UTC")
        .with_label("team", "messaging")
        .with_network("net-a")
        .with_network_alias("bus")
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
    EXPECT_EQ(generic.network_aliases()[0], "bus");
    EXPECT_TRUE(generic.reuse());
    EXPECT_EQ(generic.startup_timeout(), std::chrono::seconds(90));
    EXPECT_EQ(generic.startup_attempts(), 2);
}
