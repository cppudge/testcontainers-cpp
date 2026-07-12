#include <gtest/gtest.h>

#include <string>

#include "testcontainers/ExecResult.hpp"
#include "testcontainers/modules/MySQLContainer.hpp"

#include "EngineGuard.hpp"
#include "TempPaths.hpp"

// Tests in this file (integration; require a Linux-containers Docker daemon):
//   MySQLModule.DefaultsBootAndConnect - defaults boot; the in-container mysql client connects over TCP as the test user AND as root (shared password); connection_string() renders mysql://test:test@host:port/test.
//   MySQLModule.CustomCredsAndOrderedInitScripts - custom user/password/database (the managed env matrix beating a raw with_env duplicate, live); a host-file schema script and an inline seed script run in registration order before start() returns.
//   MySQLModule.RootOnlyModes - with_username("root") boots without a MYSQL_USER key; the passwordless-root variant boots via MYSQL_ALLOW_EMPTY_PASSWORD and connects without -p.
//   MySQLModule.CharsetCommandArg - with_command_arg("--character-set-server=utf8mb4") reaches the running server (SHOW VARIABLES).

using namespace testcontainers;
using modules::MySQLContainer;
using modules::StartedMySQL;

// Requires a Linux-containers daemon; skipped otherwise.
class MySQLModule : public tcit::LinuxEngineTest {};

TEST_F(MySQLModule, DefaultsBootAndConnect) {
    const StartedMySQL db = MySQLContainer().start();

    // -h127.0.0.1 forces TCP — the same path the readiness probe proved.
    const ExecResult select = db.container().exec(
        {"mysql", "-h127.0.0.1", "-utest", "-ptest", "-Dtest", "-N", "-B", "-e", "SELECT 1"});
    EXPECT_EQ(select.exit_code, 0);
    EXPECT_EQ(select.stdout_data, "1\n");

    // Root shares the password by construction.
    EXPECT_EQ(db.root_password(), db.password());
    const ExecResult as_root = db.container().exec(
        {"mysql", "-h127.0.0.1", "-uroot", "-ptest", "-N", "-B", "-e", "SELECT 1"});
    EXPECT_EQ(as_root.exit_code, 0);

    const std::string origin = db.host() + ":" + std::to_string(db.port());
    EXPECT_EQ(db.connection_string(), "mysql://test:test@" + origin + "/test");
}

TEST_F(MySQLModule, CustomCredsAndOrderedInitScripts) {
    const tcit::TempFile schema("CREATE TABLE orders_t(id int);", "tc_mysql_", ".sql");

    const StartedMySQL db = MySQLContainer()
                                // A raw duplicate of a managed key: the module's
                                // matrix lands after it, so "orders" must win in
                                // the running server (the -Dorders exec below is
                                // the live proof).
                                .with_env("MYSQL_DATABASE", "raw")
                                .with_username("app")
                                .with_password("s3cr3t")
                                .with_database("orders")
                                .with_init_script(schema.path()) // runs first (0000-)
                                .with_init_script("seed.sql",    // runs second (0001-)
                                                  "INSERT INTO orders_t VALUES (1), (2);")
                                .start();

    // start() returned => both scripts already ran, in registration order.
    const ExecResult r =
        db.container().exec({"mysql", "-h127.0.0.1", "-uapp", "-ps3cr3t", "-Dorders", "-N", "-B",
                             "-e", "SELECT count(*) FROM orders_t"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.stdout_data, "2\n");
}

TEST_F(MySQLModule, RootOnlyModes) {
    // Root with a password: no separate user is provisioned.
    const StartedMySQL rooted = MySQLContainer().with_username("root").start();
    EXPECT_EQ(rooted.username(), "root");
    const ExecResult as_root = rooted.container().exec(
        {"mysql", "-h127.0.0.1", "-uroot", "-ptest", "-N", "-B", "-e", "SELECT 1"});
    EXPECT_EQ(as_root.exit_code, 0);

    // Passwordless root (MYSQL_ALLOW_EMPTY_PASSWORD): connects without -p.
    const StartedMySQL open = MySQLContainer().with_username("root").with_password("").start();
    const ExecResult no_pass =
        open.container().exec({"mysql", "-h127.0.0.1", "-uroot", "-N", "-B", "-e", "SELECT 1"});
    EXPECT_EQ(no_pass.exit_code, 0);
    const std::string origin = open.host() + ":" + std::to_string(open.port());
    EXPECT_EQ(open.connection_string(), "mysql://root@" + origin + "/test");
}

TEST_F(MySQLModule, CharsetCommandArg) {
    const StartedMySQL db =
        MySQLContainer().with_command_arg("--character-set-server=utf8mb4").start();

    const ExecResult r =
        db.container().exec({"mysql", "-h127.0.0.1", "-utest", "-ptest", "-N", "-B", "-e",
                             "SHOW VARIABLES LIKE 'character_set_server'"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_NE(r.stdout_data.find("utf8mb4"), std::string::npos);
}
