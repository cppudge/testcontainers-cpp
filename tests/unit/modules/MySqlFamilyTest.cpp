#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/modules/MariaDBContainer.hpp"
#include "testcontainers/modules/MySQLContainer.hpp"

// Tests in this file (unit; no Docker daemon — the MySQL/MariaDB pair's
// shared rendering rules via to_generic()):
//   MySqlFamilyConfig.MySQLDefaultsRenderMatrixProbeAndPin - the default MySQL config renders the pinned image, exposes 3306/tcp, appends the four-key credential matrix, installs the TCP-forced mysqladmin ping probe (500ms poll), and keeps the doubled 120s budget.
//   MySqlFamilyConfig.MariaDBDefaultsRenderFlavor - the MariaDB flavor swaps in MARIADB_* env names and the image's own healthcheck.sh probe.
//   MySqlFamilyConfig.RootOnlyModeOmitsUserEnv - with_username("Root") normalizes to "root" (accounts are case-sensitive) and emits root-password env only — no *_USER key (the images refuse user=root).
//   MySqlFamilyConfig.PasswordlessRootSetsAllowEmpty - root + empty password emits the flavor's allow-empty key ("yes") and drops -p from the MySQL probe.
//   MySqlFamilyConfig.InvalidCredentialsThrowFast - a non-root user with an empty password and an empty username throw Error at render, per flavor.
//   MySqlFamilyConfig.ManagedMatrixAppendedLastWinsOverRawEnv - a raw with_env duplicate of a managed key is kept but the module's entry lands after it (bash entrypoints apply the last duplicate), and distinct credential values map to their distinct env keys.
//   MySqlFamilyConfig.EmptyDatabaseOmitsEnvKey - with_database("") emits no *_DATABASE key.
//   MySqlFamilyConfig.InitScriptsAndConfigFilesStageOrderedAndValidated - init scripts get NNNN- registration-order prefixes and .sh mode 0755; config files land in /etc/mysql/conf.d; unknown init extensions and non-.cnf config names throw.
//   MySqlFamilyConfig.CommandArgsBecomeCmd - with_command_arg / with_command_args (the batch twin) values interleave in call order and become the container cmd verbatim (the entrypoint forwards '-'-prefixed args to the server).
//   MySqlFamilyConfig.CustomWaitReplacesProbeAndCustomizerWins - with_wait drops the default probe; a customizer runs last and its settings win.
//   MySqlFamilyConfig.RenderingIsIdempotent - repeated to_generic() calls render equal env/cmd/copies.

using namespace testcontainers;
using modules::MariaDBContainer;
using modules::MySQLContainer;

namespace {

/// LAST value of `key` in the rendered env list (what a bash entrypoint sees).
std::string env_last_value(const GenericImage& generic, const std::string& key) {
    std::string value;
    for (const auto& [k, v] : generic.env()) {
        if (k == key) {
            value = v;
        }
    }
    return value;
}

bool env_has_key(const GenericImage& generic, const std::string& key) {
    for (const auto& [k, v] : generic.env()) {
        if (k == key) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(MySqlFamilyConfig, MySQLDefaultsRenderMatrixProbeAndPin) {
    const GenericImage generic = MySQLContainer().to_generic();

    EXPECT_EQ(generic.image(), "mysql");
    EXPECT_EQ(generic.tag(), "8.4");
    ASSERT_EQ(generic.exposed_ports().size(), 1u);
    EXPECT_EQ(generic.exposed_ports()[0], tcp(3306));
    EXPECT_EQ(generic.startup_timeout(), std::chrono::seconds(120));

    EXPECT_EQ(env_last_value(generic, "MYSQL_USER"), "test");
    EXPECT_EQ(env_last_value(generic, "MYSQL_PASSWORD"), "test");
    EXPECT_EQ(env_last_value(generic, "MYSQL_ROOT_PASSWORD"), "test"); // root shares the password
    EXPECT_EQ(env_last_value(generic, "MYSQL_DATABASE"), "test");

    ASSERT_EQ(generic.waits().size(), 1u);
    const auto* command = std::get_if<wait_for::Command>(&generic.waits()[0]);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->cmd,
              (std::vector<std::string>{"mysqladmin", "ping", "-h127.0.0.1", "-utest", "-ptest"}));
    EXPECT_EQ(command->poll_interval, std::chrono::milliseconds(500));
}

TEST(MySqlFamilyConfig, MariaDBDefaultsRenderFlavor) {
    const GenericImage generic = MariaDBContainer().to_generic();

    EXPECT_EQ(generic.image(), "mariadb");
    EXPECT_EQ(generic.tag(), "11");
    EXPECT_EQ(env_last_value(generic, "MARIADB_USER"), "test");
    EXPECT_EQ(env_last_value(generic, "MARIADB_PASSWORD"), "test");
    EXPECT_EQ(env_last_value(generic, "MARIADB_ROOT_PASSWORD"), "test");
    EXPECT_EQ(env_last_value(generic, "MARIADB_DATABASE"), "test");
    EXPECT_FALSE(env_has_key(generic, "MYSQL_USER")); // flavor names, not MySQL's

    ASSERT_EQ(generic.waits().size(), 1u);
    const auto* command = std::get_if<wait_for::Command>(&generic.waits()[0]);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->cmd,
              (std::vector<std::string>{"healthcheck.sh", "--connect", "--innodb_initialized"}));
}

TEST(MySqlFamilyConfig, RootOnlyModeOmitsUserEnv) {
    MySQLContainer cfg;
    cfg.with_username("Root");
    // Normalized: the real account is 'root' and account names are
    // case-sensitive — a "Root" spelling must not leak into the probe/DSN.
    EXPECT_EQ(cfg.username(), "root");

    const GenericImage generic = cfg.to_generic();
    EXPECT_FALSE(env_has_key(generic, "MYSQL_USER")); // the image refuses user=root
    EXPECT_FALSE(env_has_key(generic, "MYSQL_PASSWORD"));
    EXPECT_EQ(env_last_value(generic, "MYSQL_ROOT_PASSWORD"), "test");
    EXPECT_EQ(env_last_value(generic, "MYSQL_DATABASE"), "test");
}

TEST(MySqlFamilyConfig, PasswordlessRootSetsAllowEmpty) {
    const GenericImage mysql =
        MySQLContainer().with_username("root").with_password("").to_generic();
    EXPECT_EQ(env_last_value(mysql, "MYSQL_ALLOW_EMPTY_PASSWORD"), "yes");
    EXPECT_FALSE(env_has_key(mysql, "MYSQL_ROOT_PASSWORD"));
    // No password, no -p in the probe.
    const auto* command = std::get_if<wait_for::Command>(&mysql.waits()[0]);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->cmd,
              (std::vector<std::string>{"mysqladmin", "ping", "-h127.0.0.1", "-uroot"}));

    // MariaDB spells the allow-empty key differently.
    const GenericImage mariadb =
        MariaDBContainer().with_username("root").with_password("").to_generic();
    EXPECT_EQ(env_last_value(mariadb, "MARIADB_ALLOW_EMPTY_ROOT_PASSWORD"), "yes");
    EXPECT_FALSE(env_has_key(mariadb, "MARIADB_ROOT_PASSWORD"));
}

TEST(MySqlFamilyConfig, InvalidCredentialsThrowFast) {
    EXPECT_THROW(MySQLContainer().with_password("").to_generic(), Error);
    EXPECT_THROW(MariaDBContainer().with_password("").to_generic(), Error);
    EXPECT_THROW(MySQLContainer().with_username("").to_generic(), Error);
    EXPECT_THROW(MariaDBContainer().with_username("").to_generic(), Error);
}

TEST(MySqlFamilyConfig, ManagedMatrixAppendedLastWinsOverRawEnv) {
    const GenericImage generic = MySQLContainer()
                                     .with_env("MYSQL_ROOT_PASSWORD", "raw")
                                     .with_password("s3cr3t")
                                     .to_generic();

    // The raw duplicate is kept, but the module's entry comes after it — the
    // bash entrypoint applies the last occurrence.
    EXPECT_EQ(env_last_value(generic, "MYSQL_ROOT_PASSWORD"), "s3cr3t");

    // Distinct values land on their distinct keys (a user/password key swap
    // would be invisible with the all-"test" defaults).
    const GenericImage mapped =
        MySQLContainer().with_username("u1").with_password("p1").with_database("d1").to_generic();
    EXPECT_EQ(env_last_value(mapped, "MYSQL_USER"), "u1");
    EXPECT_EQ(env_last_value(mapped, "MYSQL_PASSWORD"), "p1");
    EXPECT_EQ(env_last_value(mapped, "MYSQL_ROOT_PASSWORD"), "p1");
    EXPECT_EQ(env_last_value(mapped, "MYSQL_DATABASE"), "d1");
}

TEST(MySqlFamilyConfig, EmptyDatabaseOmitsEnvKey) {
    const GenericImage generic = MySQLContainer().with_database("").to_generic();
    EXPECT_FALSE(env_has_key(generic, "MYSQL_DATABASE"));
}

TEST(MySqlFamilyConfig, InitScriptsAndConfigFilesStageOrderedAndValidated) {
    MySQLContainer cfg;
    cfg.with_init_script("z.sql", "CREATE TABLE t(id int);");
    cfg.with_init_script("a.sh", "#!/bin/sh\nexit 0\n");
    cfg.with_config_file(std::filesystem::path("testdata/tuning.cnf"));

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.copy_to_sources().size(), 3u);
    // Registration order beats name order: the 0000- prefix pins z before a.
    EXPECT_EQ(generic.copy_to_sources()[0].target(), "/docker-entrypoint-initdb.d/0000-z.sql");
    EXPECT_EQ(generic.copy_to_sources()[1].target(), "/docker-entrypoint-initdb.d/0001-a.sh");
    EXPECT_EQ(generic.copy_to_sources()[0].mode(), 0644);
    EXPECT_EQ(generic.copy_to_sources()[1].mode(), 0755); // .sh runs standalone
    EXPECT_EQ(generic.copy_to_sources()[2].target(), "/etc/mysql/conf.d/tuning.cnf");

    EXPECT_THROW(cfg.with_init_script("notes.txt", "SELECT 1;"), Error);
    EXPECT_THROW(cfg.with_init_script("dir/x.sql", "SELECT 1;"), Error);
    EXPECT_THROW(cfg.with_config_file(std::filesystem::path("tuning.conf")), Error);
}

TEST(MySqlFamilyConfig, CommandArgsBecomeCmd) {
    EXPECT_TRUE(MySQLContainer().to_generic().cmd().empty());

    const GenericImage generic = MySQLContainer()
                                     .with_command_arg("--character-set-server=utf8mb4")
                                     .with_command_arg("--collation-server=utf8mb4_unicode_ci")
                                     .to_generic();
    EXPECT_EQ(generic.cmd(), (std::vector<std::string>{"--character-set-server=utf8mb4",
                                                       "--collation-server=utf8mb4_unicode_ci"}));

    // The batch twin interleaves with the single form in call order (same
    // spelling pair as Redis).
    const GenericImage mariadb = MariaDBContainer()
                                     .with_command_arg("--max-connections=42")
                                     .with_command_args({"--skip-name-resolve", "--general-log=1"})
                                     .to_generic();
    EXPECT_EQ(mariadb.cmd(), (std::vector<std::string>{"--max-connections=42",
                                                       "--skip-name-resolve", "--general-log=1"}));
}

TEST(MySqlFamilyConfig, CustomWaitReplacesProbeAndCustomizerWins) {
    MySQLContainer cfg;
    cfg.with_wait(wait_for::log("ready"));
    ASSERT_EQ(cfg.to_generic().waits().size(), 1u);
    EXPECT_TRUE(std::holds_alternative<wait_for::LogMessage>(cfg.to_generic().waits()[0]));

    MariaDBContainer customized;
    customized.with_customizer([](GenericImage& generic) {
        // Sees the rendered state and has the last word.
        EXPECT_FALSE(generic.env().empty());
        generic.with_label("team", "db");
    });
    const GenericImage generic = customized.to_generic();
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
}

TEST(MySqlFamilyConfig, RenderingIsIdempotent) {
    MySQLContainer cfg;
    cfg.with_password("pw").with_command_arg("--skip-name-resolve");
    cfg.with_init_script("seed.sql", "SELECT 1;");

    const GenericImage first = cfg.to_generic();
    const GenericImage second = cfg.to_generic();
    EXPECT_EQ(first.env(), second.env());
    EXPECT_EQ(first.cmd(), second.cmd());
    EXPECT_EQ(first.copy_to_sources().size(), second.copy_to_sources().size());
    EXPECT_EQ(second.waits().size(), 1u);
}
