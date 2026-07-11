#include <gtest/gtest.h>

#include <string>

#include "testcontainers/ExecResult.hpp"
#include "testcontainers/modules/MariaDBContainer.hpp"

#include "EngineGuard.hpp"
#include "TempPaths.hpp"

// Tests in this file (integration; require a Linux-containers Docker daemon):
//   MariaDBModule.DefaultsBootAndConnect - defaults boot (implicitly proving the healthcheck.sh wait); the in-container `mariadb` client connects over TCP; connection_string() renders the mysql:// scheme.
//   MariaDBModule.InitScriptAndConfigFile - an init script and a .cnf drop-in both reach the running server (table present, variable changed).

using namespace testcontainers;
using modules::MariaDBContainer;
using modules::StartedMariaDB;

// Requires a Linux-containers daemon; skipped otherwise.
class MariaDBModule : public ::testing::Test {
protected:
    void SetUp() override {
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

TEST_F(MariaDBModule, DefaultsBootAndConnect) {
    const StartedMariaDB db = MariaDBContainer().start();

    // The image's client binaries carry the mariadb-prefixed names.
    const ExecResult select = db.container().exec(
        {"mariadb", "-h127.0.0.1", "-utest", "-ptest", "-Dtest", "-N", "-B", "-e", "SELECT 1"});
    EXPECT_EQ(select.exit_code, 0);
    EXPECT_EQ(select.stdout_data, "1\n");

    // The mysql scheme on purpose: MariaDB speaks the MySQL wire protocol
    // and URL-parsing clients widely reject "mariadb://".
    const std::string origin = db.host() + ":" + std::to_string(db.port());
    EXPECT_EQ(db.connection_string(), "mysql://test:test@" + origin + "/test");
}

TEST_F(MariaDBModule, InitScriptAndConfigFile) {
    const tcit::TempFile tuning("[mysqld]\nmax_connections=77\n", "tc_mariadb_", ".cnf");

    const StartedMariaDB db =
        MariaDBContainer()
            .with_init_script("schema.sql", "CREATE TABLE t(v int); INSERT INTO t VALUES (7);")
            .with_config_file(tuning.path())
            .start();

    const ExecResult table = db.container().exec({"mariadb", "-h127.0.0.1", "-utest", "-ptest",
                                                  "-Dtest", "-N", "-B", "-e", "SELECT v FROM t"});
    EXPECT_EQ(table.exit_code, 0);
    EXPECT_EQ(table.stdout_data, "7\n");

    const ExecResult variable =
        db.container().exec({"mariadb", "-h127.0.0.1", "-utest", "-ptest", "-N", "-B", "-e",
                             "SHOW VARIABLES LIKE 'max_connections'"});
    EXPECT_EQ(variable.exit_code, 0);
    EXPECT_NE(variable.stdout_data.find("77"), std::string::npos);
}
