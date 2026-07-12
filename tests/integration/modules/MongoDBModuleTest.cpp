#include <gtest/gtest.h>

#include <string>

#include "testcontainers/ExecResult.hpp"
#include "testcontainers/modules/MongoDB.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Linux-containers Docker daemon):
//   MongoDBModule.BecomesWritablePrimary - start() returns only once the node is the writable PRIMARY of the configured set (hello + rs.status via the mongosh helper).
//   MongoDBModule.ConnectionStringShape - the DSN is mongodb://host:port/db?directConnection=true; the per-call overload swaps the database; an empty name keeps the mandatory '/'; replicaSet= never leaks.
//   MongoDBModule.InsertFindRoundTrip - a write issued immediately after start() is accepted (PRIMARY really was reached) and read back.
//   MongoDBModule.TransactionCommitAndAbort - the replica-set payoff: a committed multi-document transaction lands, an aborted one does not.
//   MongoDBModule.CustomReplicaSetName - with_replica_set_name reaches rs.status().set, and the DSN shape is unchanged.

using namespace testcontainers;
using modules::MongoDBContainer;
using modules::MongoDBImage;

// Requires a Linux-containers daemon; skipped otherwise.
class MongoDBModule : public tcit::LinuxEngineTest {};

TEST_F(MongoDBModule, BecomesWritablePrimary) {
    const MongoDBContainer mongo = MongoDBImage().start();

    const ExecResult hello = mongo.mongosh("print(db.hello().isWritablePrimary)");
    EXPECT_EQ(hello.exit_code, 0) << hello.stderr_data;
    EXPECT_EQ(hello.stdout_data, "true\n");

    const ExecResult set = mongo.mongosh("print(rs.status().set)");
    EXPECT_EQ(set.exit_code, 0);
    EXPECT_EQ(set.stdout_data, "rs0\n");
    EXPECT_EQ(mongo.replica_set_name(), "rs0");
}

TEST_F(MongoDBModule, ConnectionStringShape) {
    const MongoDBContainer mongo = MongoDBImage().with_database("orders").start();

    const std::string origin = mongo.host() + ":" + std::to_string(mongo.port());
    EXPECT_EQ(mongo.connection_string(), "mongodb://" + origin + "/orders?directConnection=true");
    EXPECT_EQ(mongo.connection_string("other"),
              "mongodb://" + origin + "/other?directConnection=true");
    // The '/' stays even with no database: strict URI parsers reject
    // options without it.
    EXPECT_EQ(mongo.connection_string(""), "mongodb://" + origin + "/?directConnection=true");
    // Never emit replicaSet= — it would flip drivers back into discovery
    // mode, chasing the container-internal member address.
    EXPECT_EQ(mongo.connection_string().find("replicaSet"), std::string::npos);
}

TEST_F(MongoDBModule, InsertFindRoundTrip) {
    const MongoDBContainer mongo = MongoDBImage().start();

    // Immediately after start(): if PRIMARY had not been reached, the write
    // would be rejected with NotWritablePrimary.
    const ExecResult insert = mongo.mongosh("db.events.insertOne({kind: 'seed'})");
    EXPECT_EQ(insert.exit_code, 0) << insert.stderr_data;

    const ExecResult count = mongo.mongosh("print(db.events.countDocuments({}))");
    EXPECT_EQ(count.exit_code, 0);
    EXPECT_EQ(count.stdout_data, "1\n");
}

TEST_F(MongoDBModule, TransactionCommitAndAbort) {
    const MongoDBContainer mongo = MongoDBImage().start();

    // Multi-document transactions are the reason the module always runs a
    // replica set — a standalone mongod rejects startTransaction outright.
    const ExecResult txn =
        mongo.mongosh("db.createCollection('txns');"
                      "const s = db.getMongo().startSession();"
                      "const col = s.getDatabase('test').txns;"
                      "s.startTransaction(); col.insertOne({k: 1}); s.commitTransaction();"
                      "s.startTransaction(); col.insertOne({k: 2}); s.abortTransaction();"
                      "print(db.txns.countDocuments({}))");
    EXPECT_EQ(txn.exit_code, 0) << txn.stderr_data;
    // The committed insert landed, the aborted one did not — and under
    // --quiet nothing but the final print() writes to stdout, so the match
    // is exact (a substring check would let a wrong count like 11 slip by).
    EXPECT_EQ(txn.stdout_data, "1\n");
}

TEST_F(MongoDBModule, CustomReplicaSetName) {
    const MongoDBContainer mongo = MongoDBImage().with_replica_set_name("tcrs").start();

    const ExecResult set = mongo.mongosh("print(rs.status().set)");
    EXPECT_EQ(set.exit_code, 0);
    EXPECT_EQ(set.stdout_data, "tcrs\n");
    // The set name never leaks into the DSN — clients connect directly.
    EXPECT_EQ(mongo.connection_string().find("tcrs"), std::string::npos);
}
