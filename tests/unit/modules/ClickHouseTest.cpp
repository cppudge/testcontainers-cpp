#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/modules/ClickHouse.hpp"

// Tests in this file (unit; no Docker daemon — the module's rendering rules
// via to_generic()):
//   ClickHouseModuleConfig.DefaultRendersPinPortsCredsAndReadinessTriple - the default config renders the pinned image, exposes 8123+9000/tcp, appends the test/test/test credential trio, installs the ordered handover -> /ping -> SELECT 1 readiness triple, and owns no cmd.
//   ClickHouseModuleConfig.CredentialTrioAppendedLastWinsOverRawEnv - the CLICKHOUSE_* trio lands after pass-through env, so the module's values are the last duplicates (what the image's bash entrypoint applies).
//   ClickHouseModuleConfig.EmptyCredentialFieldsThrowAtRender - an empty username, database, or password throws at render, before any daemon contact.
//   ClickHouseModuleConfig.InitScriptsStageOrderedAndValidated - init scripts get the zero-padded registration prefix under /docker-entrypoint-initdb.d, .sh ships executable, the clickhouse whitelist rejects .sql.xz/.sql.zst (which the postgres set allows), and in-memory names must be bare.
//   ClickHouseModuleConfig.ConfigFilesStageValidated - config drop-ins land under /etc/clickhouse-server/config.d with their own name; only .xml/.yaml/.yml pass.
//   ClickHouseModuleConfig.CustomWaitReplacesDefaultProbe - the first with_wait drops the module's /ping probe; further calls accumulate in order.
//   ClickHouseModuleConfig.CustomizerRunsLastAndWins - a customizer sees the module-rendered builder and what it sets overrides the module's rendering.
//   ClickHouseModuleConfig.WithImageRewritesReference - with_image swaps name and tag (bare name defaults to "latest") while the rest of the config survives.
//   ClickHouseModuleConfig.RenderingIsIdempotent - to_generic() renders into a copy: repeated renders are equal and the config never accumulates env, copies, or waits.
//   ClickHouseModuleConfig.PassThroughsLandOnTheImage - the env/label/network/alias/reuse/timeout/attempts pass-throughs reach the rendered builder.

using namespace testcontainers;
using modules::ClickHouseImage;

TEST(ClickHouseModuleConfig, DefaultRendersPinPortsCredsAndReadinessTriple) {
    const GenericImage generic = ClickHouseImage().to_generic();

    EXPECT_EQ(generic.image(), "clickhouse");
    EXPECT_EQ(generic.tag(), "26.3");
    ASSERT_EQ(generic.exposed_ports().size(), 2u);
    EXPECT_EQ(generic.exposed_ports()[0], tcp(8123));
    EXPECT_EQ(generic.exposed_ports()[1], tcp(9000));

    ASSERT_EQ(generic.env().size(), 3u);
    EXPECT_EQ(generic.env()[0], (std::pair<std::string, std::string>{"CLICKHOUSE_USER", "test"}));
    EXPECT_EQ(generic.env()[1],
              (std::pair<std::string, std::string>{"CLICKHOUSE_PASSWORD", "test"}));
    EXPECT_EQ(generic.env()[2], (std::pair<std::string, std::string>{"CLICKHOUSE_DB", "test"}));

    // Ordered triple: the PID-1 handover check is what tells the real server
    // from the first-boot provisioning server (network probes cannot); /ping
    // then proves the published mapping end to end; the exec probe proves
    // the native listener and the credentials.
    ASSERT_EQ(generic.waits().size(), 3u);
    const auto* handover = std::get_if<wait_for::Command>(&generic.waits()[0]);
    ASSERT_NE(handover, nullptr);
    ASSERT_EQ(handover->cmd.size(), 3u);
    EXPECT_EQ(handover->cmd[2], "grep -q clickhouse /proc/1/comm");
    const auto* http = std::get_if<wait_for::Http>(&generic.waits()[1]);
    ASSERT_NE(http, nullptr);
    EXPECT_EQ(http->path, "/ping");
    EXPECT_EQ(http->port, tcp(8123));
    EXPECT_EQ(http->expected_status, 200);
    const auto* probe = std::get_if<wait_for::Command>(&generic.waits()[2]);
    ASSERT_NE(probe, nullptr);
    EXPECT_EQ(probe->cmd,
              (std::vector<std::string>{"clickhouse-client", "--user", "test", "--password", "test",
                                        "--database", "test", "--query", "SELECT 1"}));

    // The image's own entrypoint command keeps applying.
    EXPECT_TRUE(generic.cmd().empty());
    EXPECT_TRUE(generic.copy_to_sources().empty());
}

TEST(ClickHouseModuleConfig, CredentialTrioAppendedLastWinsOverRawEnv) {
    ClickHouseImage cfg;
    cfg.with_env("CLICKHOUSE_USER", "shadowed").with_env("TZ", "UTC").with_username("app");

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.env().size(), 5u);
    // Pass-through entries first, the managed trio last: the image's bash
    // entrypoint applies the LAST duplicate of a key.
    EXPECT_EQ(generic.env()[0].second, "shadowed");
    EXPECT_EQ(generic.env()[2], (std::pair<std::string, std::string>{"CLICKHOUSE_USER", "app"}));
    EXPECT_EQ(generic.env()[4].first, "CLICKHOUSE_DB");
}

TEST(ClickHouseModuleConfig, EmptyCredentialFieldsThrowAtRender) {
    EXPECT_THROW(ClickHouseImage().with_username("").to_generic(), Error);
    EXPECT_THROW(ClickHouseImage().with_database("").to_generic(), Error);
    EXPECT_THROW(ClickHouseImage().with_password("").to_generic(), Error);
}

TEST(ClickHouseModuleConfig, InitScriptsStageOrderedAndValidated) {
    ClickHouseImage cfg;
    cfg.with_init_script("schema.sql", "CREATE TABLE t (x Int32) ENGINE = Memory")
        .with_init_script("seed.sh", "#!/bin/sh\ntrue\n");

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.copy_to_sources().size(), 2u);
    EXPECT_EQ(generic.copy_to_sources()[0].target(), "/docker-entrypoint-initdb.d/0000-schema.sql");
    EXPECT_EQ(generic.copy_to_sources()[1].target(), "/docker-entrypoint-initdb.d/0001-seed.sh");
    EXPECT_EQ(generic.copy_to_sources()[1].mode(), 0755); // .sh ships executable

    // The clickhouse entrypoint executes .sql/.sql.gz/.sh only — the xz/zst
    // forms the postgres set allows are rejected here, not skipped silently.
    EXPECT_THROW(ClickHouseImage().with_init_script("a.sql.xz", "-"), Error);
    EXPECT_THROW(ClickHouseImage().with_init_script("a.sql.zst", "-"), Error);
    EXPECT_THROW(ClickHouseImage().with_init_script("a.txt", "-"), Error);
    EXPECT_THROW(ClickHouseImage().with_init_script("dir/a.sql", "-"), Error);
}

TEST(ClickHouseModuleConfig, ConfigFilesStageValidated) {
    ClickHouseImage cfg;
    cfg.with_config_file("relative/tuning.yml").with_config_file("override.xml");

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.copy_to_sources().size(), 2u);
    EXPECT_EQ(generic.copy_to_sources()[0].target(), "/etc/clickhouse-server/config.d/tuning.yml");
    EXPECT_TRUE(generic.copy_to_sources()[0].is_file());
    EXPECT_EQ(generic.copy_to_sources()[1].target(),
              "/etc/clickhouse-server/config.d/override.xml");

    EXPECT_THROW(ClickHouseImage().with_config_file("tuning.conf"), Error);
    EXPECT_THROW(ClickHouseImage().with_config_file("tuning.json"), Error);
}

TEST(ClickHouseModuleConfig, CustomWaitReplacesDefaultProbe) {
    ClickHouseImage cfg;
    cfg.with_wait(wait_for::log("Ready for connections"));
    cfg.with_wait(wait_for::listening_port(tcp(9000)));

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.waits().size(), 2u); // the default readiness triple is gone
    EXPECT_NE(std::get_if<wait_for::LogMessage>(&generic.waits()[0]), nullptr);
    EXPECT_NE(std::get_if<wait_for::Port>(&generic.waits()[1]), nullptr);
}

TEST(ClickHouseModuleConfig, CustomizerRunsLastAndWins) {
    ClickHouseImage cfg;
    cfg.with_customizer([](GenericImage& generic) {
           // Runs after the module's rendering, so the env trio is applied.
           ASSERT_EQ(generic.env().size(), 3u);
           generic.with_label("team", "analytics");
       })
        .with_customizer([](GenericImage& generic) { generic.with_env("TZ", "UTC"); });

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.env().size(), 4u); // customizer env lands after the trio
    EXPECT_EQ(generic.env()[3].first, "TZ");
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
}

TEST(ClickHouseModuleConfig, WithImageRewritesReference) {
    ClickHouseImage cfg;
    cfg.with_username("app").with_image("mirror.example/clickhouse:25.8");

    const GenericImage generic = cfg.to_generic();
    EXPECT_EQ(generic.image(), "mirror.example/clickhouse");
    EXPECT_EQ(generic.tag(), "25.8");
    // The rest of the config survives the swap.
    ASSERT_EQ(generic.env().size(), 3u);
    EXPECT_EQ(generic.env()[0].second, "app");

    cfg.with_image("clickhouse"); // bare name defaults to "latest"
    EXPECT_EQ(cfg.to_generic().image(), "clickhouse");
    EXPECT_EQ(cfg.to_generic().tag(), "latest");
}

TEST(ClickHouseModuleConfig, RenderingIsIdempotent) {
    ClickHouseImage cfg;
    cfg.with_init_script("schema.sql", "CREATE TABLE t (x Int32) ENGINE = Memory")
        .with_config_file("tuning.yml");

    const GenericImage first = cfg.to_generic();
    const GenericImage second = cfg.to_generic();
    EXPECT_EQ(first.env(), second.env());
    ASSERT_EQ(second.copy_to_sources().size(), 2u); // staged once, never re-appended
    ASSERT_EQ(second.waits().size(), 3u);           // the readiness triple is rendered once
}

TEST(ClickHouseModuleConfig, PassThroughsLandOnTheImage) {
    ClickHouseImage cfg;
    cfg.with_env("TZ", "UTC")
        .with_label("team", "analytics")
        .with_network("net-a")
        .with_network_alias("olap")
        .with_reuse()
        .with_startup_timeout(std::chrono::seconds(90))
        .with_startup_attempts(2);

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.env().size(), 4u); // TZ + the managed trio
    EXPECT_EQ(generic.env()[0].first, "TZ");
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
    EXPECT_EQ(generic.network().value_or(""), "net-a");
    ASSERT_EQ(generic.network_aliases().size(), 1u);
    EXPECT_EQ(generic.network_aliases()[0], "olap");
    EXPECT_TRUE(generic.reuse());
    EXPECT_EQ(generic.startup_timeout(), std::chrono::seconds(90));
    EXPECT_EQ(generic.startup_attempts(), 2);
}
