#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/Network.hpp"
#include "testcontainers/modules/Kafka.hpp"

#include "EngineGuard.hpp"
#include "WaitStrategies.hpp"

// Tests in this file (integration; require a Linux-containers Docker daemon):
//   KafkaModule.StartsAndExposesBootstrap - the two-phase boot completes; bootstrap_servers() is host():port() (bare, schemeless), the published port accepts TCP, and internal_bootstrap_servers() falls back to the short container id.
//   KafkaModule.AdvertisedListenersCarryMappedPort - the money test for the choreography: the broker's runtime config advertises PLAINTEXT://host:mapped-port (the value that did not exist until after start).
//   KafkaModule.ProduceConsumeRoundTrip - a message produced and consumed through the in-container CLI (bootstrapping the INTERNAL listener) round-trips.
//   KafkaModule.WithTopicPreCreatesPartitions - with_topic("multi", 3) exists with 3 partitions right after start().
//   KafkaModule.TwoContainersOverNetwork - a peer container on a shared network produces and consumes via the advertised internal listener <alias>:9093 (DNS + metadata-directed second connection, end to end).

using namespace testcontainers;
using modules::KafkaContainer;
using modules::KafkaImage;

// Requires a Linux-containers daemon; skipped otherwise.
class KafkaModule : public tcit::LinuxEngineTest {};

TEST_F(KafkaModule, StartsAndExposesBootstrap) {
    const KafkaContainer kafka = KafkaImage().start();

    EXPECT_EQ(kafka.bootstrap_servers(),
              kafka.host() + ":" + std::to_string(kafka.port())); // bare, no PLAINTEXT://
    EXPECT_TRUE(detail::tcp_probe(kafka.host(), kafka.port(), std::chrono::milliseconds(5000)));

    // No network alias configured: the internal authority falls back to the
    // short container id.
    EXPECT_EQ(kafka.internal_bootstrap_servers(), kafka.container().id().substr(0, 12) + ":9093");
    EXPECT_EQ(kafka.cluster_id(), "4L6g3nShT-eMCtK--X86sw");
}

TEST_F(KafkaModule, AdvertisedListenersCarryMappedPort) {
    const KafkaContainer kafka = KafkaImage().start();

    // The broker's runtime view of its own config must carry the REAL mapped
    // port — the value that did not exist until after start_container.
    const ExecResult res = kafka.container().exec(
        {"/opt/kafka/bin/kafka-configs.sh", "--bootstrap-server", "localhost:9093", "--entity-type",
         "brokers", "--entity-name", "1", "--describe", "--all"});
    ASSERT_EQ(res.exit_code, 0);
    const std::string expected =
        "advertised.listeners=PLAINTEXT://" + kafka.bootstrap_servers() + ",BROKER://";
    EXPECT_NE(res.stdout_data.find(expected), std::string::npos)
        << "runtime config did not carry the mapped port; describe output:\n"
        << res.stdout_data.substr(0, 2000);
}

TEST_F(KafkaModule, ProduceConsumeRoundTrip) {
    const KafkaContainer kafka = KafkaImage().start();

    // The pipe lives INSIDE the container (sh -c) — no exec-stdin, so this
    // works on every transport. Bootstrap at the internal listener: the
    // metadata answered on :9092 advertises the host-side address,
    // unreachable from inside the container.
    const ExecResult produced =
        kafka.container().exec({"sh", "-c",
                                "printf 'hello-tc\\n' | /opt/kafka/bin/kafka-console-producer.sh "
                                "--bootstrap-server localhost:9093 --topic rt"});
    ASSERT_EQ(produced.exit_code, 0) << produced.stderr_data;

    const ExecResult consumed = kafka.container().exec(
        {"/opt/kafka/bin/kafka-console-consumer.sh", "--bootstrap-server", "localhost:9093",
         "--topic", "rt", "--from-beginning", "--max-messages", "1", "--timeout-ms", "30000"});
    EXPECT_EQ(consumed.exit_code, 0) << consumed.stderr_data;
    EXPECT_NE(consumed.stdout_data.find("hello-tc"), std::string::npos);
}

TEST_F(KafkaModule, WithTopicPreCreatesPartitions) {
    const KafkaContainer kafka = KafkaImage().with_topic("multi", 3).start();

    const ExecResult res =
        kafka.container().exec({"/opt/kafka/bin/kafka-topics.sh", "--bootstrap-server",
                                "localhost:9093", "--describe", "--topic", "multi"});
    ASSERT_EQ(res.exit_code, 0) << res.stderr_data;
    EXPECT_NE(res.stdout_data.find("PartitionCount: 3"), std::string::npos)
        << res.stdout_data.substr(0, 1000);
}

TEST_F(KafkaModule, TwoContainersOverNetwork) {
    Network net = Network::builder().create();

    const KafkaContainer kafka = KafkaImage().with_network(net).with_network_alias("kafka").start();
    EXPECT_EQ(kafka.internal_bootstrap_servers(), "kafka:9093");

    // A plain client box from the SAME image (no extra pull), idling.
    const Container client_box = GenericImage("apache/kafka", "3.9.1")
                                     .with_network(net)
                                     .with_cmd({"sh", "-c", "while true; do sleep 60; done"})
                                     .start();

    // From the peer: produce and consume via the advertised internal
    // listener. This proves network DNS AND the metadata-directed second
    // connection end to end from a peer's perspective.
    const ExecResult produced =
        client_box.exec({"sh", "-c",
                         "printf 'over-network\\n' | /opt/kafka/bin/kafka-console-producer.sh "
                         "--bootstrap-server kafka:9093 --topic net-rt"});
    ASSERT_EQ(produced.exit_code, 0) << produced.stderr_data;

    const ExecResult consumed = client_box.exec(
        {"/opt/kafka/bin/kafka-console-consumer.sh", "--bootstrap-server", "kafka:9093", "--topic",
         "net-rt", "--from-beginning", "--max-messages", "1", "--timeout-ms", "30000"});
    EXPECT_EQ(consumed.exit_code, 0) << consumed.stderr_data;
    EXPECT_NE(consumed.stdout_data.find("over-network"), std::string::npos);
}
