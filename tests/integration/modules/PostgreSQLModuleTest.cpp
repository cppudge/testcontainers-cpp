#include <gtest/gtest.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <cstdint>
#include <string>

#include "testcontainers/ExecResult.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/modules/PostgreSQL.hpp"

#include "EngineGuard.hpp"
#include "TempPaths.hpp"
#include "TestEnv.hpp"

// Tests in this file (integration; require a Linux-containers Docker daemon):
//   PostgreSQLModule.DefaultsStartAndConnect - defaults start; exec_sql works immediately after start() (the readiness regression a socket probe would flake on); DSN forms render test/test/test, incl. the connection_string_with_scheme variant.
//   PostgreSQLModule.TcpProbeSurvivesInitWindow - slow init scripts (pg_sleep) finish before start() returns: the TCP probe cannot false-positive through the temp-server window, so the schema is queryable immediately.
//   PostgreSQLModule.InitScriptsRunInRegistrationOrder - a script registered first but named last ("z.sql" before "a.sql") runs first: registration order beats the entrypoint's name order.
//   PostgreSQLModule.InitScriptFromHostFile - a host .sql file is copied into initdb.d and applied.
//   PostgreSQLModule.CustomCredentialsDsnAndConninfo - custom user/password/database reach the server; connection_string percent-encodes the password; conninfo quotes it per libpq rules.
//   PostgreSQLModule.ConfigOptionsReachServer - with_config_option("max_connections","42") is visible via SHOW.
//   PostgreSQLModule.CustomizerReachesCreateBody - a customizer-added label is visible in the created container's inspect.
//   PostgreSQLModule.HostSidePgHandshake - a raw protocol-v3 startup packet to host():port() gets a real PostgreSQL reply byte (proves the published port reaches the server, which a bare TCP connect cannot on Docker Desktop).
//   PostgreSQLModule.ReuseAdoptsSeededServer - with reuse enabled, a second start() adopts the same container: data seeded after the first start survives and init scripts are not re-run.

using namespace testcontainers;
using modules::PostgreSQLContainer;
using modules::PostgreSQLImage;

namespace {

/// Send a PostgreSQL protocol-v3 StartupMessage and return the first reply
/// byte: 'R' (authentication request) or 'E' (error) — either proves a real
/// PostgreSQL answered on the published port. Docker Desktop's host proxy
/// accepts TCP on any published port, so a bare connect proves nothing.
char pg_first_reply_byte(const std::string& host, std::uint16_t port, const std::string& user,
                         const std::string& database) {
    namespace asio = boost::asio;
    using asio::ip::tcp;

    asio::io_context io;
    tcp::resolver resolver(io);
    const auto endpoints = resolver.resolve(host, std::to_string(port));
    tcp::socket socket(io);
    asio::connect(socket, endpoints);

    std::string params;
    params += "user";
    params += '\0';
    params += user;
    params += '\0';
    params += "database";
    params += '\0';
    params += database;
    params += '\0';
    params += '\0';

    std::string message;
    const auto append_u32 = [&message](std::uint32_t value) {
        message += static_cast<char>((value >> 24) & 0xFF);
        message += static_cast<char>((value >> 16) & 0xFF);
        message += static_cast<char>((value >> 8) & 0xFF);
        message += static_cast<char>(value & 0xFF);
    };
    append_u32(static_cast<std::uint32_t>(8 + params.size())); // length, self included
    append_u32(196608);                                        // protocol 3.0
    message += params;

    asio::write(socket, asio::buffer(message));

    char reply = 0;
    boost::system::error_code ec;
    socket.read_some(asio::buffer(&reply, 1), ec);
    return reply;
}

} // namespace

// Requires a Linux-containers daemon; skipped otherwise.
class PostgreSQLModule : public tcit::LinuxEngineTest {};

TEST_F(PostgreSQLModule, DefaultsStartAndConnect) {
    const PostgreSQLContainer pg = PostgreSQLImage().start();

    // Immediately after start(): a socket-based readiness probe would flake
    // here (the init window), the TCP probe must not.
    const ExecResult r = pg.exec_sql("SELECT 1");
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.stdout_data, "1\n");

    EXPECT_EQ(pg.username(), "test");
    EXPECT_EQ(pg.password(), "test");
    EXPECT_EQ(pg.database(), "test");
    const std::string origin = pg.host() + ":" + std::to_string(pg.port());
    EXPECT_EQ(pg.connection_string(), "postgresql://test:test@" + origin + "/test");
    EXPECT_EQ(pg.connection_string_with_scheme("postgres"),
              "postgres://test:test@" + origin + "/test");
    EXPECT_EQ(pg.conninfo(), "host=" + pg.host() + " port=" + std::to_string(pg.port()) +
                                 " dbname=test user=test password=test");
}

TEST_F(PostgreSQLModule, TcpProbeSurvivesInitWindow) {
    const PostgreSQLContainer pg =
        PostgreSQLImage()
            .with_init_script("slow.sql", "SELECT pg_sleep(3);")
            .with_init_script("schema.sql",
                              "CREATE TABLE t(id int); INSERT INTO t VALUES (1), (2);")
            .start();

    // start() returned => every init script finished and the REAL server
    // (the only one that listens on TCP) is up.
    const ExecResult r = pg.exec_sql("SELECT count(*) FROM t");
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.stdout_data, "2\n");
}

TEST_F(PostgreSQLModule, InitScriptsRunInRegistrationOrder) {
    // "z.sql" is registered first but sorts last — if name order won, "a.sql"
    // would insert into a table that does not exist yet and the boot would
    // fail (ON_ERROR the entrypoint aborts).
    const PostgreSQLContainer pg =
        PostgreSQLImage()
            .with_init_script("z.sql", "CREATE TABLE ordered(v text); "
                                       "INSERT INTO ordered VALUES ('z');")
            .with_init_script("a.sql", "INSERT INTO ordered VALUES ('a');")
            .start();

    const ExecResult r = pg.exec_sql("SELECT count(*) FROM ordered");
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.stdout_data, "2\n");
}

TEST_F(PostgreSQLModule, InitScriptFromHostFile) {
    const tcit::TempFile script("CREATE TABLE fromhost(id int); "
                                "INSERT INTO fromhost VALUES (42);",
                                "tc_pg_", ".sql");

    const PostgreSQLContainer pg = PostgreSQLImage().with_init_script(script.path()).start();

    const ExecResult r = pg.exec_sql("SELECT id FROM fromhost");
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.stdout_data, "42\n");
}

TEST_F(PostgreSQLModule, CustomCredentialsDsnAndConninfo) {
    const PostgreSQLContainer pg = PostgreSQLImage()
                                       .with_username("app")
                                       .with_password("p@ss w'rd")
                                       .with_database("orders")
                                       .start();

    const ExecResult who = pg.exec_sql("SELECT current_user || '|' || current_database()");
    EXPECT_EQ(who.exit_code, 0);
    EXPECT_EQ(who.stdout_data, "app|orders\n");

    // '@' -> %40, ' ' -> %20, '\'' -> %27 in the URI form...
    EXPECT_NE(pg.connection_string().find("app:p%40ss%20w%27rd@"), std::string::npos);
    // ...and libpq single-quote escaping in the keyword/value form.
    EXPECT_NE(pg.conninfo().find("password='p@ss w\\'rd'"), std::string::npos);
}

TEST_F(PostgreSQLModule, ConfigOptionsReachServer) {
    const PostgreSQLContainer pg =
        PostgreSQLImage().with_config_option("max_connections", "42").start();

    const ExecResult r = pg.exec_sql("SHOW max_connections");
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.stdout_data, "42\n");
}

TEST_F(PostgreSQLModule, CustomizerReachesCreateBody) {
    const std::string marker = "tc-pg-customizer-" + tcit::random_suffix();

    const PostgreSQLContainer pg = PostgreSQLImage()
                                       .with_customizer([&marker](GenericImage& generic) {
                                           generic.with_label("tc.test.marker", marker);
                                       })
                                       .start();

    EXPECT_NE(pg.container().inspect_raw().find(marker), std::string::npos);
}

TEST_F(PostgreSQLModule, HostSidePgHandshake) {
    const PostgreSQLContainer pg = PostgreSQLImage().start();

    const char reply = pg_first_reply_byte(pg.host(), pg.port(), "test", "test");
    // 'R' = authentication request — a real PostgreSQL answered end to end.
    EXPECT_EQ(reply, 'R');
}

TEST_F(PostgreSQLModule, ReuseAdoptsSeededServer) {
    // Enable reuse for this test only (RAII restore covers every exit path,
    // including gtest fatal asserts, which return without throwing).
    const tctest::ScopedEnv reuse_enable("TESTCONTAINERS_REUSE_ENABLE", "true");

    PostgreSQLImage cfg;
    cfg.with_reuse(true)
        // A unique marker so this run's config can't collide with a stale
        // container left over from an earlier run.
        .with_label("tc.pg.reuse.marker", "pg-reuse-" + tcit::random_suffix())
        .with_init_script("seed.sql", "CREATE TABLE seed(v int); "
                                      "INSERT INTO seed VALUES (1), (2);");

    std::string adopted_id;
    try {
        const PostgreSQLContainer first = cfg.start();
        adopted_id = first.container().id();
        ASSERT_FALSE(adopted_id.empty());
        EXPECT_TRUE(first.container().is_persistent());
        EXPECT_EQ(first.exec_sql("INSERT INTO seed VALUES (3)").exit_code, 0);

        // The second identical start adopts the running server: same id, the
        // post-start row still present, init scripts NOT re-run (else the
        // count would have reset to 2).
        const PostgreSQLContainer second = cfg.start();
        EXPECT_EQ(second.container().id(), adopted_id);
        const ExecResult r = second.exec_sql("SELECT count(*) FROM seed");
        EXPECT_EQ(r.exit_code, 0);
        EXPECT_EQ(r.stdout_data, "3\n");
    } catch (...) {
        // Manual cleanup: reuse handles are persistent and never auto-remove.
        if (!adopted_id.empty()) {
            try {
                DockerClient::from_environment().remove_container(adopted_id, true, true);
            } catch (...) {
                // Best-effort: rethrowing the original failure matters more.
            }
        }
        throw;
    }

    ASSERT_FALSE(adopted_id.empty());
    EXPECT_NO_THROW(DockerClient::from_environment().remove_container(adopted_id, true, true));
}
