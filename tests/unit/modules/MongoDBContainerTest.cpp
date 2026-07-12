#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/modules/MongoDBContainer.hpp"

// Tests in this file (unit; no Docker daemon — the module's rendering rules
// via to_generic()):
//   MongoDBModuleConfig.DefaultRendersRsCmdWaitsAndHook - the default config renders the pinned image, exposes 27017, sets cmd {--replSet, rs0, --bind_ip_all}, installs the ordered waits (log line, then listening port), registers exactly one started hook (the initiate + PRIMARY choreography), and sets NO env (MONGO_INITDB_* is a boot-breaker under --replSet).
//   MongoDBModuleConfig.DefaultsAndSettersReflectGetters - the builder defaults to rs0/test and the domain setters record into the matching getters.
//   MongoDBModuleConfig.ReplicaSetNameValidatedAtRender - a name outside [A-Za-z0-9_-] (which is quoted into the initiate JS) throws Error at render; a valid custom name lands in the command line.
//   MongoDBModuleConfig.CustomizerRunsLastAndWins - a customizer sees the rendered builder and its settings win.
//   MongoDBModuleConfig.EnvPassThroughValidatedAgainstInitdbRoot - with_env/with_label land on the builder, and the boot-breaking MONGO_INITDB_ROOT_* keys throw at render.
//   MongoDBModuleConfig.WithImageRewritesReference - with_image swaps the reference while the choreography survives.
//   MongoDBModuleConfig.RenderingIsIdempotent - repeated to_generic() calls render equal cmd/waits/hooks (nothing accumulates).

using namespace testcontainers;
using modules::MongoDBContainer;

TEST(MongoDBModuleConfig, DefaultRendersRsCmdWaitsAndHook) {
    const GenericImage generic = MongoDBContainer().to_generic();

    EXPECT_EQ(generic.image(), "mongo");
    EXPECT_EQ(generic.tag(), "7");
    ASSERT_EQ(generic.exposed_ports().size(), 1u);
    EXPECT_EQ(generic.exposed_ports()[0], tcp(27017));

    EXPECT_EQ(generic.cmd(), (std::vector<std::string>{"--replSet", "rs0", "--bind_ip_all"}));
    EXPECT_TRUE(generic.env().empty()); // MONGO_INITDB_* would break the boot

    ASSERT_EQ(generic.waits().size(), 2u);
    const auto* log = std::get_if<wait_for::LogMessage>(&generic.waits()[0]);
    ASSERT_NE(log, nullptr);
    EXPECT_EQ(log->text, "Waiting for connections");
    EXPECT_TRUE(std::holds_alternative<wait_for::Port>(generic.waits()[1]));

    EXPECT_EQ(generic.started_hooks().size(), 1u); // initiate + PRIMARY wait
}

TEST(MongoDBModuleConfig, DefaultsAndSettersReflectGetters) {
    MongoDBContainer cfg;
    EXPECT_EQ(cfg.replica_set_name(), "rs0");
    EXPECT_EQ(cfg.database(), "test");

    cfg.with_replica_set_name("tcrs").with_database("orders");
    EXPECT_EQ(cfg.replica_set_name(), "tcrs");
    EXPECT_EQ(cfg.database(), "orders");
}

TEST(MongoDBModuleConfig, ReplicaSetNameValidatedAtRender) {
    // The name is single-quoted into the initiate JS — reject anything that
    // could escape it.
    EXPECT_THROW(MongoDBContainer().with_replica_set_name("rs0'; quit()").to_generic(), Error);
    EXPECT_THROW(MongoDBContainer().with_replica_set_name("").to_generic(), Error);

    const GenericImage generic =
        MongoDBContainer().with_replica_set_name("rs-orders_1").to_generic();
    EXPECT_EQ(generic.cmd()[1], "rs-orders_1");
}

TEST(MongoDBModuleConfig, CustomizerRunsLastAndWins) {
    MongoDBContainer cfg;
    cfg.with_customizer([](GenericImage& generic) {
        // Sees the rendered state (the choreography is already in place).
        ASSERT_EQ(generic.waits().size(), 2u);
        generic.with_label("team", "storage");
    });

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
}

TEST(MongoDBModuleConfig, EnvPassThroughValidatedAgainstInitdbRoot) {
    const GenericImage generic =
        MongoDBContainer().with_env("TZ", "UTC").with_label("team", "storage").to_generic();
    ASSERT_EQ(generic.env().size(), 1u);
    EXPECT_EQ(generic.env()[0].first, "TZ");
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");

    // Auth without a cluster keyfile refuses to start under --replSet — the
    // module rejects the boot-breaking keys up front.
    EXPECT_THROW(MongoDBContainer().with_env("MONGO_INITDB_ROOT_USERNAME", "root").to_generic(),
                 Error);
    EXPECT_THROW(MongoDBContainer().with_env("MONGO_INITDB_ROOT_PASSWORD", "pw").to_generic(),
                 Error);
}

TEST(MongoDBModuleConfig, WithImageRewritesReference) {
    const GenericImage generic = MongoDBContainer().with_image("mongo:8").to_generic();
    EXPECT_EQ(generic.image(), "mongo");
    EXPECT_EQ(generic.tag(), "8");
    EXPECT_EQ(generic.cmd()[0], "--replSet"); // the choreography survives the swap
}

TEST(MongoDBModuleConfig, RenderingIsIdempotent) {
    MongoDBContainer cfg;
    cfg.with_replica_set_name("tcrs").with_database("orders");

    const GenericImage first = cfg.to_generic();
    const GenericImage second = cfg.to_generic();
    EXPECT_EQ(first.cmd(), second.cmd());
    EXPECT_EQ(second.waits().size(), 2u);
    EXPECT_EQ(second.started_hooks().size(), 1u);
}
