#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/modules/KafkaContainer.hpp"

#include "modules/KafkaDetail.hpp"

// Tests in this file (unit; no Docker daemon — the module's rendering rules
// via to_generic() and the pure detail helpers):
//   KafkaModuleConfig.DefaultRendersEnvPlaceholderSentinelAndHook - the default config renders the pinned image, exposes 9092 only, emits the complete KRaft env set (no KAFKA_ADVERTISED_LISTENERS), installs the placeholder command + sentinel log wait, and registers exactly one started hook.
//   KafkaModuleConfig.UserEnvAppendedAfterModuleEnv - with_env entries land after the module's, so a duplicate key resolves to the user's value in the image's bash launch script.
//   KafkaModuleConfig.ClusterIdValidatedAtRender - a malformed cluster id throws Error at to_generic(); a valid override lands in CLUSTER_ID.
//   KafkaModuleConfig.TopicsRenderReuseVisibleLabel - with_topic renders the org.testcontainers.kafka.topics label (name:partitions, registration order); no topics, no label; an empty name or non-positive partition count throws at render.
//   KafkaModuleConfig.CustomizerRunsLastAndWins - a customizer sees the rendered builder and its settings win.
//   KafkaDetail.ClusterIdValidation - exactly 22 URL-safe base64 chars pass; wrong length and non-URL-safe chars are rejected.
//   KafkaDetail.StarterScriptAdvertisesBothListeners - the generated script exports PLAINTEXT with the real mapped port and BROKER with the internal host (never CONTROLLER), and execs the apache run script with the confluent fallback.
//   KafkaDetail.AwaitCommandGatesOnStarterPath - the placeholder command echoes the sentinel, polls for the starter path, and execs it.

using namespace testcontainers;
using modules::KafkaContainer;

namespace {

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

TEST(KafkaModuleConfig, DefaultRendersEnvPlaceholderSentinelAndHook) {
    const GenericImage generic = KafkaContainer().to_generic();

    EXPECT_EQ(generic.image(), "apache/kafka");
    EXPECT_EQ(generic.tag(), "3.9.1");
    ASSERT_EQ(generic.exposed_ports().size(), 1u);
    EXPECT_EQ(generic.exposed_ports()[0], tcp(9092)); // 9093/9094 stay unpublished

    EXPECT_EQ(env_last_value(generic, "KAFKA_PROCESS_ROLES"), "broker,controller");
    EXPECT_EQ(env_last_value(generic, "KAFKA_CONTROLLER_QUORUM_VOTERS"), "1@localhost:9094");
    EXPECT_EQ(env_last_value(generic, "KAFKA_INTER_BROKER_LISTENER_NAME"), "BROKER");
    EXPECT_EQ(env_last_value(generic, "CLUSTER_ID"), "4L6g3nShT-eMCtK--X86sw");
    // The starter script owns the advertised listeners — they need the mapped
    // port, which does not exist at create time.
    EXPECT_FALSE(env_has_key(generic, "KAFKA_ADVERTISED_LISTENERS"));

    ASSERT_EQ(generic.cmd().size(), 3u); // {"sh", "-c", <await loop>}
    EXPECT_EQ(generic.cmd()[0], "sh");
    EXPECT_NE(generic.cmd()[2].find("testcontainers_start.sh"), std::string::npos);

    ASSERT_EQ(generic.waits().size(), 1u);
    const auto* log = std::get_if<wait_for::LogMessage>(&generic.waits()[0]);
    ASSERT_NE(log, nullptr);
    EXPECT_EQ(log->text, "testcontainers: waiting for start script");

    EXPECT_EQ(generic.started_hooks().size(), 1u); // the boot choreography
    EXPECT_TRUE(generic.labels().empty());         // no topics, no label
}

TEST(KafkaModuleConfig, UserEnvAppendedAfterModuleEnv) {
    const GenericImage generic = KafkaContainer()
                                     .with_env("KAFKA_NUM_PARTITIONS", "3")
                                     .with_env("KAFKA_OFFSETS_TOPIC_NUM_PARTITIONS", "7")
                                     .to_generic();

    EXPECT_EQ(env_last_value(generic, "KAFKA_NUM_PARTITIONS"), "3");
    // The duplicate of a module key resolves to the USER's value (the image's
    // bash launch script applies the last occurrence).
    EXPECT_EQ(env_last_value(generic, "KAFKA_OFFSETS_TOPIC_NUM_PARTITIONS"), "7");
}

TEST(KafkaModuleConfig, ClusterIdValidatedAtRender) {
    EXPECT_THROW(KafkaContainer().with_cluster_id("too-short").to_generic(), Error);
    EXPECT_THROW(KafkaContainer().with_cluster_id("invalid+chars/not-ok==").to_generic(), Error);

    KafkaContainer cfg;
    cfg.with_cluster_id("AAAAAAAAAAAAAAAAAAAAAA");
    EXPECT_EQ(cfg.cluster_id(), "AAAAAAAAAAAAAAAAAAAAAA");
    EXPECT_EQ(env_last_value(cfg.to_generic(), "CLUSTER_ID"), "AAAAAAAAAAAAAAAAAAAAAA");
}

TEST(KafkaModuleConfig, TopicsRenderReuseVisibleLabel) {
    const GenericImage generic =
        KafkaContainer().with_topic("orders", 3).with_topic("audit").to_generic();

    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "org.testcontainers.kafka.topics");
    EXPECT_EQ(generic.labels()[0].second, "orders:3,audit:1");

    // Fail fast instead of an opaque topic-creation error at start().
    EXPECT_THROW(KafkaContainer().with_topic("bad", 0).to_generic(), Error);
    EXPECT_THROW(KafkaContainer().with_topic("", 1).to_generic(), Error);
}

TEST(KafkaModuleConfig, CustomizerRunsLastAndWins) {
    KafkaContainer cfg;
    cfg.with_customizer([](GenericImage& generic) {
        // Sees the rendered state (placeholder cmd already installed)...
        ASSERT_EQ(generic.cmd().size(), 3u);
        generic.with_label("team", "streaming");
    });

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
}

TEST(KafkaDetail, ClusterIdValidation) {
    using modules::detail::valid_kafka_cluster_id;
    EXPECT_TRUE(valid_kafka_cluster_id("4L6g3nShT-eMCtK--X86sw"));
    EXPECT_TRUE(valid_kafka_cluster_id("AAAAAAAAAAAAAAAAAAAA_-"));
    EXPECT_FALSE(valid_kafka_cluster_id(""));
    EXPECT_FALSE(valid_kafka_cluster_id("AAAAAAAAAAAAAAAAAAAAA"));   // 21
    EXPECT_FALSE(valid_kafka_cluster_id("AAAAAAAAAAAAAAAAAAAAAAA")); // 23
    EXPECT_FALSE(valid_kafka_cluster_id("AAAAAAAAAAAAAAAAAAAA+/"));  // plain base64, not URL-safe
    EXPECT_FALSE(valid_kafka_cluster_id("AAAAAAAAAAAAAAAAAAAAA="));  // padding
}

TEST(KafkaDetail, StarterScriptAdvertisesBothListeners) {
    const std::string script = modules::detail::kafka_starter_script("localhost", 32771, "kafka");

    EXPECT_NE(script.find("export KAFKA_ADVERTISED_LISTENERS="
                          "'PLAINTEXT://localhost:32771,BROKER://kafka:9093'"),
              std::string::npos);
    EXPECT_EQ(script.find("CONTROLLER://"), std::string::npos); // KRaft forbids advertising it
    EXPECT_NE(script.find("exec /etc/kafka/docker/run"), std::string::npos);
    EXPECT_NE(script.find("exec /etc/confluent/docker/run"), std::string::npos); // fallback
}

TEST(KafkaDetail, AwaitCommandGatesOnStarterPath) {
    const std::string cmd = modules::detail::kafka_await_command();

    EXPECT_NE(cmd.find("echo 'testcontainers: waiting for start script'"), std::string::npos);
    EXPECT_NE(cmd.find("while [ ! -f /tmp/testcontainers_start.sh ]"), std::string::npos);
    EXPECT_NE(cmd.find("exec /tmp/testcontainers_start.sh"), std::string::npos);
}
