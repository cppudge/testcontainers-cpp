#include <gtest/gtest.h>

#include <string>

#include "testcontainers/ExecResult.hpp"
#include "testcontainers/modules/Mosquitto.hpp"

#include "EngineGuard.hpp"
#include "MqttConnect.hpp"

// Tests in this file (integration; require a Linux-containers Docker daemon):
//   MosquittoModule.StartsServesAndBuildsUrl - a default MosquittoImage starts, a raw MQTT 3.1.1 CONNECT on host()/port() gets an accepting CONNACK (anonymous remote clients really reach the broker), and broker_url() renders tcp://host:port.
//   MosquittoModule.ExecPubSubRoundTrip - a retained publish then a bounded subscribe through the in-image mosquitto_pub/mosquitto_sub round-trips a message (the no-driver behavioral proof).
//   MosquittoModule.SysTopicReportsPinnedVersion - the broker-retained $SYS/broker/version topic reports the pinned 2.0 line (pin-drift canary; argv exec, so $SYS sees no shell).
//   MosquittoModule.ConfigOptionReachesTheBroker - with_config_option("retain_available","false") is parsed by the real broker: the retained round trip now times out (mosquitto_sub -W exits 27 with no payload).
//   MosquittoModule.ConfigReplaceOwnsTheContract - a with_config_content replacement really replaces the managed config: allow_anonymous false makes the same raw CONNECT come back "not authorized" (return code 5).

using namespace testcontainers;
using modules::MosquittoContainer;
using modules::MosquittoImage;

// Requires a Linux-containers daemon; skipped otherwise.
class MosquittoModule : public tcit::LinuxEngineTest {};

TEST_F(MosquittoModule, StartsServesAndBuildsUrl) {
    const MosquittoContainer broker = MosquittoImage().start();

    const std::string ack = tcit::mqtt_connect(broker.host(), broker.port());
    ASSERT_EQ(ack.size(), 4u);
    EXPECT_EQ(static_cast<unsigned char>(ack[0]), 0x20); // CONNACK
    EXPECT_EQ(static_cast<unsigned char>(ack[3]), 0x00); // connection accepted

    EXPECT_EQ(broker.broker_url(), "tcp://" + broker.host() + ":" + std::to_string(broker.port()));
}

TEST_F(MosquittoModule, ExecPubSubRoundTrip) {
    const MosquittoContainer broker = MosquittoImage().start();

    // Retained: the subscribe-after-publish delivery needs no background
    // exec racing a blocking subscriber.
    EXPECT_EQ(
        broker.container().exec({"mosquitto_pub", "-t", "tc/rt", "-m", "ping", "-r"}).exit_code, 0);

    const ExecResult sub =
        broker.container().exec({"mosquitto_sub", "-t", "tc/rt", "-C", "1", "-W", "5"});
    EXPECT_EQ(sub.exit_code, 0);
    EXPECT_NE(sub.stdout_data.find("ping"), std::string::npos);
}

TEST_F(MosquittoModule, SysTopicReportsPinnedVersion) {
    const MosquittoContainer broker = MosquittoImage().start();

    const ExecResult sub = broker.container().exec(
        {"mosquitto_sub", "-t", "$SYS/broker/version", "-C", "1", "-W", "5"});
    EXPECT_EQ(sub.exit_code, 0);
    EXPECT_NE(sub.stdout_data.find("mosquitto version 2.0"), std::string::npos);
}

TEST_F(MosquittoModule, ConfigOptionReachesTheBroker) {
    const MosquittoContainer broker =
        MosquittoImage().with_config_option("retain_available", "false").start();

    // The publish may or may not report the dropped connection (QoS-0
    // publishers can exit before noticing); the observable is the subscriber:
    // nothing was retained, so the bounded wait times out (exit 27, no payload).
    broker.container().exec({"mosquitto_pub", "-t", "tc/rt", "-m", "ping", "-r"});

    const ExecResult sub =
        broker.container().exec({"mosquitto_sub", "-t", "tc/rt", "-C", "1", "-W", "2"});
    EXPECT_EQ(sub.exit_code, 27); // mosquitto_sub: the -W timeout expired
    EXPECT_EQ(sub.stdout_data.find("ping"), std::string::npos);
}

TEST_F(MosquittoModule, ConfigReplaceOwnsTheContract) {
    const MosquittoContainer broker =
        MosquittoImage().with_config_content("listener 1883\nallow_anonymous false\n").start();

    const std::string ack = tcit::mqtt_connect(broker.host(), broker.port());
    ASSERT_EQ(ack.size(), 4u);
    EXPECT_EQ(static_cast<unsigned char>(ack[0]), 0x20); // CONNACK
    EXPECT_EQ(static_cast<unsigned char>(ack[3]), 0x05); // refused: not authorized
}
