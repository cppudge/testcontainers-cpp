#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/modules/RustFS.hpp"

// Tests in this file (unit; no Docker daemon — the module's rendering rules
// via to_generic()):
//   RustFSModuleConfig.DefaultRendersPinPortsCredsAndHealthWait - the default config renders the pinned beta image, exposes 9000+9001/tcp, appends the rustfsadmin credential pair, installs the /health probe, and owns no command (the image's entrypoint boots the server on its own).
//   RustFSModuleConfig.CredentialPairAppendedLastWinsOverRawEnv - the RUSTFS_* pair lands after pass-through env, so the module's values are the last duplicates (what the image's shell entrypoint applies).
//   RustFSModuleConfig.EmptyCredentialFieldsThrowAtRender - an empty access or secret key throws at render, before any daemon contact (the server itself imposes no length rules).
//   RustFSModuleConfig.CustomWaitReplacesDefaultProbe - the first with_wait drops the module's /health probe; further calls accumulate in order.
//   RustFSModuleConfig.CustomizerRunsLastAndWins - a customizer sees the module-rendered builder and what it sets overrides the module's rendering.
//   RustFSModuleConfig.WithImageRewritesReference - with_image swaps name and tag (bare name defaults to "latest") while the rest of the config survives.
//   RustFSModuleConfig.RenderingIsIdempotent - to_generic() renders into a copy: repeated renders are equal and the config never accumulates env or waits.
//   RustFSModuleConfig.PassThroughsLandOnTheImage - the env/label/network/alias/reuse/timeout/attempts pass-throughs reach the rendered builder.

using namespace testcontainers;
using modules::RustFSImage;

TEST(RustFSModuleConfig, DefaultRendersPinPortsCredsAndHealthWait) {
    const GenericImage generic = RustFSImage().to_generic();

    EXPECT_EQ(generic.image(), "rustfs/rustfs");
    EXPECT_EQ(generic.tag(), "1.0.0-beta.8");
    ASSERT_EQ(generic.exposed_ports().size(), 2u);
    EXPECT_EQ(generic.exposed_ports()[0], tcp(9000));
    EXPECT_EQ(generic.exposed_ports()[1], tcp(9001));

    ASSERT_EQ(generic.env().size(), 2u);
    EXPECT_EQ(generic.env()[0],
              (std::pair<std::string, std::string>{"RUSTFS_ACCESS_KEY", "rustfsadmin"}));
    EXPECT_EQ(generic.env()[1],
              (std::pair<std::string, std::string>{"RUSTFS_SECRET_KEY", "rustfsadmin"}));

    ASSERT_EQ(generic.waits().size(), 1u);
    const auto* http = std::get_if<wait_for::Http>(&generic.waits()[0]);
    ASSERT_NE(http, nullptr);
    EXPECT_EQ(http->path, "/health");
    EXPECT_EQ(http->port, tcp(9000));
    EXPECT_EQ(http->expected_status, 200);

    // The image's own entrypoint command keeps applying (commandless boot).
    EXPECT_TRUE(generic.cmd().empty());
}

TEST(RustFSModuleConfig, CredentialPairAppendedLastWinsOverRawEnv) {
    RustFSImage cfg;
    cfg.with_env("RUSTFS_ACCESS_KEY", "shadowed")
        .with_env("RUSTFS_CONSOLE_ENABLE", "false")
        .with_access_key("testkey");

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.env().size(), 4u);
    // Pass-through entries first, the managed pair last: the image's shell
    // entrypoint applies the LAST duplicate of a key.
    EXPECT_EQ(generic.env()[0].second, "shadowed");
    EXPECT_EQ(generic.env()[2],
              (std::pair<std::string, std::string>{"RUSTFS_ACCESS_KEY", "testkey"}));
    EXPECT_EQ(generic.env()[3].first, "RUSTFS_SECRET_KEY");
}

TEST(RustFSModuleConfig, EmptyCredentialFieldsThrowAtRender) {
    EXPECT_THROW(RustFSImage().with_access_key("").to_generic(), Error);
    EXPECT_THROW(RustFSImage().with_secret_key("").to_generic(), Error);
    // No server-side length rules: a one-character pair renders (and boots).
    EXPECT_NO_THROW(RustFSImage().with_access_key("k").with_secret_key("s").to_generic());
}

TEST(RustFSModuleConfig, CustomWaitReplacesDefaultProbe) {
    RustFSImage cfg;
    cfg.with_wait(wait_for::log("RustFS Http API:"));
    cfg.with_wait(wait_for::listening_port(tcp(9000)));

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.waits().size(), 2u); // the default /health probe is gone
    EXPECT_NE(std::get_if<wait_for::LogMessage>(&generic.waits()[0]), nullptr);
    EXPECT_NE(std::get_if<wait_for::Port>(&generic.waits()[1]), nullptr);
}

TEST(RustFSModuleConfig, CustomizerRunsLastAndWins) {
    RustFSImage cfg;
    cfg.with_customizer([](GenericImage& generic) {
           // Runs after the module's rendering, so the credential pair is
           // already applied.
           ASSERT_EQ(generic.env().size(), 2u);
           generic.with_label("team", "storage");
       })
        .with_customizer(
            [](GenericImage& generic) { generic.with_env("RUSTFS_OBS_LOGGER_LEVEL", "info"); });

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.env().size(), 3u); // customizer env lands after the pair
    EXPECT_EQ(generic.env()[2].first, "RUSTFS_OBS_LOGGER_LEVEL");
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
}

TEST(RustFSModuleConfig, WithImageRewritesReference) {
    RustFSImage cfg;
    cfg.with_access_key("testkey").with_image("rustfs/rustfs:1.0.0-beta.8-glibc");

    const GenericImage generic = cfg.to_generic();
    EXPECT_EQ(generic.image(), "rustfs/rustfs");
    EXPECT_EQ(generic.tag(), "1.0.0-beta.8-glibc");
    // The rest of the config survives the swap.
    ASSERT_EQ(generic.env().size(), 2u);
    EXPECT_EQ(generic.env()[0].second, "testkey");

    cfg.with_image("rustfs/rustfs"); // bare name defaults to "latest"
    EXPECT_EQ(cfg.to_generic().image(), "rustfs/rustfs");
    EXPECT_EQ(cfg.to_generic().tag(), "latest");
}

TEST(RustFSModuleConfig, RenderingIsIdempotent) {
    RustFSImage cfg;
    cfg.with_env("TZ", "UTC");

    const GenericImage first = cfg.to_generic();
    const GenericImage second = cfg.to_generic();
    EXPECT_EQ(first.env(), second.env());
    ASSERT_EQ(second.env().size(), 3u);   // TZ + the pair, appended once
    ASSERT_EQ(second.waits().size(), 1u); // the probe is rendered once
}

TEST(RustFSModuleConfig, PassThroughsLandOnTheImage) {
    RustFSImage cfg;
    cfg.with_env("RUSTFS_CONSOLE_ENABLE", "false")
        .with_label("team", "storage")
        .with_network("net-a")
        .with_network_alias("s3")
        .with_reuse()
        .with_startup_timeout(std::chrono::seconds(90))
        .with_startup_attempts(2);

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.env().size(), 3u); // the console knob + the managed pair
    EXPECT_EQ(generic.env()[0].first, "RUSTFS_CONSOLE_ENABLE");
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
    EXPECT_EQ(generic.network().value_or(""), "net-a");
    ASSERT_EQ(generic.network_aliases().size(), 1u);
    EXPECT_EQ(generic.network_aliases()[0], "s3");
    EXPECT_TRUE(generic.reuse());
    EXPECT_EQ(generic.startup_timeout(), std::chrono::seconds(90));
    EXPECT_EQ(generic.startup_attempts(), 2);
}
