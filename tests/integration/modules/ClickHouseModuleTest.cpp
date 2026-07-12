#include <gtest/gtest.h>

#include <string>

#include "testcontainers/ExecResult.hpp"
#include "testcontainers/modules/ClickHouse.hpp"

#include "EngineGuard.hpp"
#include "HttpGet.hpp"
#include "TempPaths.hpp"

// Tests in this file (integration; require a Linux-containers Docker daemon):
//   ClickHouseModule.DefaultsStartAndConnect - a default ClickHouseImage starts, exec_sql answers SELECT 1 immediately after start(), the getters report test/test/test, and connection_string()/http_url() render the exact endpoint strings.
//   ClickHouseModule.HttpPingFromHost - a raw host-side GET /ping through the published HTTP port answers 200 "Ok." (the end-to-end proof a bare TCP connect cannot give).
//   ClickHouseModule.InitWindowImmunity - with a deliberately slow init script the readiness probe cannot false-positive through the first-boot loopback provisioning server: the schema seeded by later scripts is queryable immediately after start().
//   ClickHouseModule.InitScriptsRunInRegistrationOrder - a script registered first runs first even when its name sorts last ("z.sql" before "a.sql").
//   ClickHouseModule.CustomCredentialsAndDsn - custom user/password/database reach the server (currentUser/currentDatabase) and the DSN percent-encodes the password.
//   ClickHouseModule.ConfigFileReachesServer - a .yml config.d drop-in is merged by the real server (system.server_settings reports the overridden max_connections).

using namespace testcontainers;
using modules::ClickHouseContainer;
using modules::ClickHouseImage;

// Requires a Linux-containers daemon; skipped otherwise.
class ClickHouseModule : public tcit::LinuxEngineTest {};

TEST_F(ClickHouseModule, DefaultsStartAndConnect) {
    const ClickHouseContainer ch = ClickHouseImage().start();

    const ExecResult one = ch.exec_sql("SELECT 1");
    EXPECT_EQ(one.exit_code, 0);
    EXPECT_EQ(one.stdout_data, "1\n");

    EXPECT_EQ(ch.username(), "test");
    EXPECT_EQ(ch.password(), "test");
    EXPECT_EQ(ch.database(), "test");
    EXPECT_EQ(ch.connection_string(), "clickhouse://test:test@" + ch.host() + ":" +
                                          std::to_string(ch.native_port()) + "/test");
    EXPECT_EQ(ch.http_url(), "http://" + ch.host() + ":" + std::to_string(ch.http_port()));
}

TEST_F(ClickHouseModule, HttpPingFromHost) {
    const ClickHouseContainer ch = ClickHouseImage().start();

    const std::string response = tcit::http_get(ch.host(), ch.http_port(), "/ping");
    EXPECT_EQ(response.substr(0, 12), "HTTP/1.1 200");
    EXPECT_NE(response.find("Ok.\n"), std::string::npos);
}

TEST_F(ClickHouseModule, InitWindowImmunity) {
    // The slow script stretches the first-boot provisioning window (the
    // temporary loopback server runs all initdb.d scripts); if the readiness
    // triple could reach that server, start() would return before the schema
    // exists and the SELECT below would fail. Two statements of ≤3s each —
    // the server caps sleepEachRow at 3s per block, and a failing init
    // script aborts the whole boot. Scripts run with NO default database, so
    // the schema qualifies its names.
    const ClickHouseContainer ch =
        ClickHouseImage()
            .with_init_script("slow.sql", "SELECT sleepEachRow(0.9) FROM numbers(3);\n"
                                          "SELECT sleepEachRow(0.9) FROM numbers(3);")
            .with_init_script("schema.sql",
                              "CREATE TABLE test.t (x Int32) ENGINE = MergeTree ORDER BY x")
            .with_init_script("seed.sql", "INSERT INTO test.t VALUES (42)")
            .start();

    const ExecResult select = ch.exec_sql("SELECT x FROM t");
    EXPECT_EQ(select.exit_code, 0);
    EXPECT_EQ(select.stdout_data, "42\n");
}

TEST_F(ClickHouseModule, InitScriptsRunInRegistrationOrder) {
    // "z.sql" (registered first) must run before "a.sql" (registered second):
    // the insert only works if the table already exists. The zero-padded
    // registration prefix beats the entrypoint's name-order execution.
    const ClickHouseContainer ch =
        ClickHouseImage()
            .with_init_script("z.sql",
                              "CREATE TABLE test.ordered_t (x Int32) ENGINE = MergeTree ORDER BY x")
            .with_init_script("a.sql", "INSERT INTO test.ordered_t VALUES (1)")
            .start();

    const ExecResult count = ch.exec_sql("SELECT count() FROM ordered_t");
    EXPECT_EQ(count.exit_code, 0);
    EXPECT_EQ(count.stdout_data, "1\n");
}

TEST_F(ClickHouseModule, CustomCredentialsAndDsn) {
    const ClickHouseContainer ch = ClickHouseImage()
                                       .with_username("app")
                                       .with_password("s3cr@t/pw")
                                       .with_database("appdb")
                                       .start();

    const ExecResult who = ch.exec_sql("SELECT currentUser(), currentDatabase()");
    EXPECT_EQ(who.exit_code, 0);
    EXPECT_EQ(who.stdout_data, "app\tappdb\n");

    EXPECT_EQ(ch.connection_string(), "clickhouse://app:s3cr%40t%2Fpw@" + ch.host() + ":" +
                                          std::to_string(ch.native_port()) + "/appdb");
}

TEST_F(ClickHouseModule, ConfigFileReachesServer) {
    const tcit::TempFile tuning("max_connections: 777\n", "tc_clickhouse_", ".yml");

    const ClickHouseContainer ch = ClickHouseImage().with_config_file(tuning.path()).start();

    const ExecResult setting =
        ch.exec_sql("SELECT value FROM system.server_settings WHERE name = 'max_connections'");
    EXPECT_EQ(setting.exit_code, 0);
    EXPECT_EQ(setting.stdout_data, "777\n");
}
