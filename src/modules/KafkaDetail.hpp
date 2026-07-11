#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Pure assembly helpers for the Kafka module — env set, placeholder command,
// generated starter script, validation. Kept free of daemon types so they
// unit-test without a daemon.

namespace testcontainers::modules::detail {

/// The sentinel the placeholder command echoes before waiting for the starter
/// script; the request-level wait keys on it (phase 1 of the Kafka boot).
inline constexpr std::string_view kKafkaSentinel = "testcontainers: waiting for start script";

/// The KRaft server's terminal boot line — phase 2's readiness marker
/// (emitted by apache/kafka and cp-kafka in KRaft mode alike).
inline constexpr std::string_view kKafkaReadyLine = "Kafka Server started";

/// The in-container path of the generated starter script.
inline constexpr std::string_view kKafkaStarterPath = "/tmp/testcontainers_start.sh";

/// The reuse-visibility label carrying the with_topic list (the topic set
/// otherwise lives only inside the started hook, invisible to the reuse hash).
inline constexpr std::string_view kKafkaTopicsLabel = "org.testcontainers.kafka.topics";

/// True for a well-formed KRaft cluster id: exactly 22 URL-safe base64
/// characters (a 16-byte UUID as kafka-storage.sh random-uuid prints it).
bool valid_kafka_cluster_id(std::string_view id);

/// The complete env set for a single-node KRaft broker. Complete matters:
/// once any user config is supplied the image drops its baked-in default
/// properties, so every mandatory key must come from here.
/// KAFKA_ADVERTISED_LISTENERS is deliberately absent — the starter script
/// exports it at boot time, once the mapped host port exists.
std::vector<std::pair<std::string, std::string>> kafka_env(const std::string& cluster_id);

/// The placeholder command body (for `sh -c`): echo the sentinel, wait for
/// the starter script to appear, exec it.
std::string kafka_await_command();

/// The generated starter script: export the advertised listeners — the REAL
/// mapped port baked in — then exec the image's launch script (apache path
/// first, confluent fallback). The CONTROLLER listener is deliberately not
/// advertised: KRaft rejects controller listeners in advertised.listeners.
std::string kafka_starter_script(const std::string& advertised_host, std::uint16_t mapped_port,
                                 const std::string& internal_host);

/// "name:partitions,..." — the topics label value, in registration order.
std::string kafka_topics_label(const std::vector<std::pair<std::string, int>>& topics);

} // namespace testcontainers::modules::detail
