#include <gtest/gtest.h>

#include <string>

#include "testcontainers/ExecResult.hpp"
#include "testcontainers/modules/ScyllaDB.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Linux-containers Docker daemon):
//   ScyllaDBModule.BecomesQueryable - a default ScyllaDBImage starts, reports a COMPLETED bootstrap through exec_cql immediately after start(), and the getters report the contact-point shape and the default datacenter.
//   ScyllaDBModule.KeyspaceTableRoundTrip - a keyspace/table/insert/select batch round-trips through one exec_cql call (NetworkTopologyStrategy: 2025.x+ tablets reject SimpleStrategy).
//   ScyllaDBModule.InitScriptsSeedBeforeStartReturns - with_init_script content is queryable immediately after start() — the post-ready script phase gates start(), and scripts run in registration order.
//   ScyllaDBModule.CustomDatacenterReported - with_datacenter renders --dc, the node reports it in system.local, and the getter agrees.

using namespace testcontainers;
using modules::ScyllaDBContainer;
using modules::ScyllaDBImage;

// Requires a Linux-containers daemon; skipped otherwise.
class ScyllaDBModule : public tcit::LinuxEngineTest {};

TEST_F(ScyllaDBModule, BecomesQueryable) {
    const ScyllaDBContainer scylla = ScyllaDBImage().start();

    const ExecResult bootstrapped = scylla.exec_cql("SELECT bootstrapped FROM system.local");
    ASSERT_EQ(bootstrapped.exit_code, 0) << bootstrapped.stderr_data;
    EXPECT_NE(bootstrapped.stdout_data.find("COMPLETED"), std::string::npos)
        << bootstrapped.stdout_data;
    EXPECT_NE(bootstrapped.stdout_data.find("(1 rows)"), std::string::npos);

    EXPECT_EQ(scylla.contact_point(), scylla.host() + ":" + std::to_string(scylla.port()));
    EXPECT_EQ(scylla.datacenter(), "datacenter1");
}

TEST_F(ScyllaDBModule, KeyspaceTableRoundTrip) {
    const ScyllaDBContainer scylla = ScyllaDBImage().start();

    // NetworkTopologyStrategy on purpose: newer releases enable tablets for
    // fresh keyspaces, and tablets reject SimpleStrategy.
    const ExecResult round =
        scylla.exec_cql("CREATE KEYSPACE tc WITH replication = "
                        "{'class': 'NetworkTopologyStrategy', 'replication_factor': 1}; "
                        "CREATE TABLE tc.kv (k text PRIMARY KEY, v int); "
                        "INSERT INTO tc.kv (k, v) VALUES ('answer', 42); "
                        "SELECT v FROM tc.kv WHERE k = 'answer';");
    ASSERT_EQ(round.exit_code, 0) << round.stderr_data;
    EXPECT_NE(round.stdout_data.find("42"), std::string::npos) << round.stdout_data;
    EXPECT_NE(round.stdout_data.find("(1 rows)"), std::string::npos);
}

TEST_F(ScyllaDBModule, InitScriptsSeedBeforeStartReturns) {
    // Two scripts prove registration order: the insert only works if the
    // schema script already ran.
    const ScyllaDBContainer scylla =
        ScyllaDBImage()
            .with_init_script("schema.cql",
                              "CREATE KEYSPACE seed WITH replication = "
                              "{'class': 'NetworkTopologyStrategy', 'replication_factor': 1};\n"
                              "CREATE TABLE seed.t (k int PRIMARY KEY);")
            .with_init_script("data.cql", "INSERT INTO seed.t (k) VALUES (7);")
            .start();

    const ExecResult select = scylla.exec_cql("SELECT k FROM seed.t");
    ASSERT_EQ(select.exit_code, 0) << select.stderr_data;
    EXPECT_NE(select.stdout_data.find('7'), std::string::npos) << select.stdout_data;
    EXPECT_NE(select.stdout_data.find("(1 rows)"), std::string::npos);
}

TEST_F(ScyllaDBModule, CustomDatacenterReported) {
    const ScyllaDBContainer scylla = ScyllaDBImage().with_datacenter("tcdc").start();

    EXPECT_EQ(scylla.datacenter(), "tcdc");
    const ExecResult dc = scylla.exec_cql("SELECT data_center FROM system.local");
    ASSERT_EQ(dc.exit_code, 0) << dc.stderr_data;
    EXPECT_NE(dc.stdout_data.find("tcdc"), std::string::npos) << dc.stdout_data;
}
