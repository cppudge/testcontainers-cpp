#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/modules/PostgreSQLContainer.hpp"

// Tests in this file (unit; no Docker daemon — the module's rendering rules
// via to_generic()):
//   PostgreSQLModuleConfig.DefaultRendersPinPortTrioAndTcpProbe - the default config renders the pinned image, exposes 5432/tcp, appends the test/test/test env trio, installs the pg_isready TCP probe with interpolated -U/-d, and leaves cmd untouched.
//   PostgreSQLModuleConfig.CredentialTrioAppendedLastWinsOverRawEnv - a raw with_env("POSTGRES_USER", ...) duplicate is kept but the module's trio lands after it (the image's bash entrypoint applies the last occurrence).
//   PostgreSQLModuleConfig.EmptyPasswordSkipsEnvKeyUnderTrust - with trust auth an empty password renders no POSTGRES_PASSWORD key at all.
//   PostgreSQLModuleConfig.ConfigOptionsRenderPostgresDashC - config options render cmd {"postgres","-c","k=v",...} in registration order; no options leave cmd empty.
//   PostgreSQLModuleConfig.InitScriptsKeepRegistrationOrderAndModes - init scripts land under /docker-entrypoint-initdb.d with 0000-/0001- prefixes in registration order (beating name order), .sh gets mode 0755, .sql stays 0644, bytes are carried verbatim.
//   PostgreSQLModuleConfig.InitScriptRejectsUnknownExtensionAndNonBareName - an unknown extension throws at registration time; a content-form name with directories throws; .sql.zst is accepted.
//   PostgreSQLModuleConfig.CustomWaitReplacesDefaultProbe - with_wait drops the module's pg_isready probe; a customizer-added wait keeps it (runs in addition).
//   PostgreSQLModuleConfig.ValidationFailsFast - empty username/database throw; an empty password throws unless POSTGRES_HOST_AUTH_METHOD=trust is set via with_env.
//   PostgreSQLModuleConfig.CustomizerRunsLastAndWins - a customizer sees the rendered builder and its settings win over the module's rendering.
//   PostgreSQLModuleConfig.RenderingIsIdempotent - repeated to_generic() calls render equal env/cmd/copies (no accumulation in the config).

using namespace testcontainers;
using modules::PostgreSQLContainer;

namespace {

/// Index of `key` in the rendered env list, or npos.
std::size_t env_index(const GenericImage& generic, const std::string& key) {
    for (std::size_t i = 0; i < generic.env().size(); ++i) {
        if (generic.env()[i].first == key) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

/// LAST value of `key` in the rendered env list (the one the daemon applies).
std::string env_last_value(const GenericImage& generic, const std::string& key) {
    std::string value;
    for (const auto& [k, v] : generic.env()) {
        if (k == key) {
            value = v;
        }
    }
    return value;
}

} // namespace

TEST(PostgreSQLModuleConfig, DefaultRendersPinPortTrioAndTcpProbe) {
    const GenericImage generic = PostgreSQLContainer().to_generic();

    EXPECT_EQ(generic.image(), "postgres");
    EXPECT_EQ(generic.tag(), "16-alpine");
    ASSERT_EQ(generic.exposed_ports().size(), 1u);
    EXPECT_EQ(generic.exposed_ports()[0], tcp(5432));

    EXPECT_EQ(env_last_value(generic, "POSTGRES_USER"), "test");
    EXPECT_EQ(env_last_value(generic, "POSTGRES_PASSWORD"), "test");
    EXPECT_EQ(env_last_value(generic, "POSTGRES_DB"), "test");

    ASSERT_EQ(generic.waits().size(), 1u);
    const auto* command = std::get_if<wait_for::Command>(&generic.waits()[0]);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->cmd, (std::vector<std::string>{"pg_isready", "-h", "127.0.0.1", "-p", "5432",
                                                      "-U", "test", "-d", "test"}));

    EXPECT_TRUE(generic.cmd().empty()); // image default preserved
}

TEST(PostgreSQLModuleConfig, CredentialTrioAppendedLastWinsOverRawEnv) {
    PostgreSQLContainer cfg;
    cfg.with_env("POSTGRES_USER", "raw").with_username("app").with_database("orders");

    const GenericImage generic = cfg.to_generic();
    // The raw duplicate is kept, but the module's entry comes AFTER it — the
    // image's bash entrypoint applies the last occurrence, so the getters
    // cannot desync.
    EXPECT_NE(env_index(generic, "POSTGRES_USER"), static_cast<std::size_t>(-1));
    EXPECT_EQ(env_last_value(generic, "POSTGRES_USER"), "app");
    EXPECT_EQ(env_last_value(generic, "POSTGRES_DB"), "orders");
    EXPECT_EQ(cfg.username(), "app");

    // The probe argv follows the module's credentials, not the raw env.
    const auto* command = std::get_if<wait_for::Command>(&generic.waits()[0]);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->cmd[6], "app");
    EXPECT_EQ(command->cmd[8], "orders");
}

TEST(PostgreSQLModuleConfig, EmptyPasswordSkipsEnvKeyUnderTrust) {
    PostgreSQLContainer cfg;
    cfg.with_password("").with_env("POSTGRES_HOST_AUTH_METHOD", "trust");

    const GenericImage generic = cfg.to_generic();
    EXPECT_EQ(env_index(generic, "POSTGRES_PASSWORD"), static_cast<std::size_t>(-1));
    EXPECT_EQ(env_last_value(generic, "POSTGRES_USER"), "test");
}

TEST(PostgreSQLModuleConfig, ConfigOptionsRenderPostgresDashC) {
    EXPECT_TRUE(PostgreSQLContainer().to_generic().cmd().empty());

    PostgreSQLContainer cfg;
    cfg.with_config_option("fsync", "off").with_config_option("wal_level", "logical");
    EXPECT_EQ(cfg.to_generic().cmd(),
              (std::vector<std::string>{"postgres", "-c", "fsync=off", "-c", "wal_level=logical"}));
}

TEST(PostgreSQLModuleConfig, InitScriptsKeepRegistrationOrderAndModes) {
    PostgreSQLContainer cfg;
    cfg.with_init_script("z.sql", "CREATE TABLE t(id int);");
    cfg.with_init_script("a.sh", "#!/bin/sh\nexit 0\n");

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.copy_to_sources().size(), 2u);
    // Registration order beats name order: the 0000- prefix pins z before a.
    EXPECT_EQ(generic.copy_to_sources()[0].target(), "/docker-entrypoint-initdb.d/0000-z.sql");
    EXPECT_EQ(generic.copy_to_sources()[1].target(), "/docker-entrypoint-initdb.d/0001-a.sh");
    EXPECT_EQ(generic.copy_to_sources()[0].mode(), 0644);
    EXPECT_EQ(generic.copy_to_sources()[1].mode(), 0755); // .sh runs standalone
    EXPECT_EQ(generic.copy_to_sources()[0].bytes(), "CREATE TABLE t(id int);");
}

TEST(PostgreSQLModuleConfig, InitScriptRejectsUnknownExtensionAndNonBareName) {
    PostgreSQLContainer cfg;
    // The entrypoint would skip a .txt silently — the worst failure mode, so
    // registration throws instead.
    EXPECT_THROW(cfg.with_init_script("notes.txt", "SELECT 1;"), Error);
    EXPECT_THROW(cfg.with_init_script(std::filesystem::path("fixtures/schema.psql")), Error);
    EXPECT_THROW(cfg.with_init_script("dir/schema.sql", "SELECT 1;"), Error);
    EXPECT_NO_THROW(cfg.with_init_script("compressed.sql.zst", "\x28\xb5\x2f\xfd"));
}

TEST(PostgreSQLModuleConfig, CustomWaitReplacesDefaultProbe) {
    PostgreSQLContainer replaced;
    replaced.with_wait(wait_for::log("ready"));
    ASSERT_EQ(replaced.to_generic().waits().size(), 1u);
    EXPECT_TRUE(std::holds_alternative<wait_for::LogMessage>(replaced.to_generic().waits()[0]));

    // A customizer-added wait runs IN ADDITION to the default probe.
    PostgreSQLContainer added;
    added.with_customizer([](GenericImage& generic) { generic.with_wait(wait_for::log("extra")); });
    const GenericImage generic = added.to_generic();
    ASSERT_EQ(generic.waits().size(), 2u);
    EXPECT_TRUE(std::holds_alternative<wait_for::Command>(generic.waits()[0]));
    EXPECT_TRUE(std::holds_alternative<wait_for::LogMessage>(generic.waits()[1]));
}

TEST(PostgreSQLModuleConfig, ValidationFailsFast) {
    EXPECT_THROW(PostgreSQLContainer().with_username("").to_generic(), Error);
    EXPECT_THROW(PostgreSQLContainer().with_database("").to_generic(), Error);
    // The image refuses an empty password; the module converts the confusing
    // 60s wait timeout into an immediate throw...
    EXPECT_THROW(PostgreSQLContainer().with_password("").to_generic(), Error);
    // ...unless trust auth was opted into explicitly.
    EXPECT_NO_THROW(PostgreSQLContainer()
                        .with_password("")
                        .with_env("POSTGRES_HOST_AUTH_METHOD", "trust")
                        .to_generic());
}

TEST(PostgreSQLModuleConfig, CustomizerRunsLastAndWins) {
    PostgreSQLContainer cfg;
    cfg.with_config_option("fsync", "off").with_customizer([](GenericImage& generic) {
        // Sees the module-rendered state...
        ASSERT_FALSE(generic.cmd().empty());
        EXPECT_EQ(generic.cmd()[0], "postgres");
        // ...and has the last word.
        generic.with_cmd({"postgres", "-c", "port=5432"});
    });

    EXPECT_EQ(cfg.to_generic().cmd(), (std::vector<std::string>{"postgres", "-c", "port=5432"}));
}

TEST(PostgreSQLModuleConfig, RenderingIsIdempotent) {
    PostgreSQLContainer cfg;
    cfg.with_username("app")
        .with_config_option("fsync", "off")
        .with_init_script("seed.sql", "SELECT 1;");

    const GenericImage first = cfg.to_generic();
    const GenericImage second = cfg.to_generic();
    EXPECT_EQ(first.env(), second.env());
    EXPECT_EQ(first.cmd(), second.cmd());
    EXPECT_EQ(first.copy_to_sources().size(), second.copy_to_sources().size());
    EXPECT_EQ(second.waits().size(), 1u);
}
