#include "KafkaDetail.hpp"

namespace testcontainers::modules::detail {

bool valid_kafka_cluster_id(std::string_view id) {
    if (id.size() != 22) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::vector<std::pair<std::string, std::string>> kafka_env(const std::string& cluster_id) {
    return {
        // Three listeners: PLAINTEXT (host-side clients, the only published
        // port), BROKER (peers on the docker network + in-container CLI,
        // doubling as the inter-broker listener), CONTROLLER (the node's own
        // KRaft quorum — never advertised).
        {"KAFKA_LISTENERS", "PLAINTEXT://0.0.0.0:9092,BROKER://0.0.0.0:9093,"
                            "CONTROLLER://0.0.0.0:9094"},
        {"KAFKA_LISTENER_SECURITY_PROTOCOL_MAP",
         "PLAINTEXT:PLAINTEXT,BROKER:PLAINTEXT,CONTROLLER:PLAINTEXT"},
        {"KAFKA_INTER_BROKER_LISTENER_NAME", "BROKER"},
        // Combined single-node KRaft: this one process is broker and
        // controller, voting for itself over the controller listener.
        {"KAFKA_PROCESS_ROLES", "broker,controller"},
        {"KAFKA_NODE_ID", "1"},
        {"KAFKA_CONTROLLER_QUORUM_VOTERS", "1@localhost:9094"},
        {"KAFKA_CONTROLLER_LISTENER_NAMES", "CONTROLLER"},
        {"CLUSTER_ID", cluster_id},
        // Single-node replication factors + test-friendly latencies: instant
        // consumer-group joins, no fsync stalls.
        {"KAFKA_OFFSETS_TOPIC_REPLICATION_FACTOR", "1"},
        {"KAFKA_OFFSETS_TOPIC_NUM_PARTITIONS", "1"},
        {"KAFKA_TRANSACTION_STATE_LOG_REPLICATION_FACTOR", "1"},
        {"KAFKA_TRANSACTION_STATE_LOG_MIN_ISR", "1"},
        {"KAFKA_GROUP_INITIAL_REBALANCE_DELAY_MS", "0"},
        {"KAFKA_LOG_FLUSH_INTERVAL_MESSAGES", "9223372036854775807"},
    };
}

std::string kafka_await_command() {
    // `sleep 0.1` works in busybox ash and bash alike; the exec keeps the
    // process chain short so a graceful stop's SIGTERM reaches the broker.
    return std::string("echo '") + std::string(kKafkaSentinel) + "'; while [ ! -f " +
           std::string(kKafkaStarterPath) + " ]; do sleep 0.1; done; exec " +
           std::string(kKafkaStarterPath);
}

std::string kafka_starter_script(const std::string& advertised_host, std::uint16_t mapped_port,
                                 const std::string& internal_host) {
    return "#!/bin/bash\n"
           "export KAFKA_ADVERTISED_LISTENERS='PLAINTEXT://" +
           advertised_host + ":" + std::to_string(mapped_port) + ",BROKER://" + internal_host +
           ":9093'\n"
           "if [ -x /etc/kafka/docker/run ]; then\n"
           "  exec /etc/kafka/docker/run\n"
           "else\n"
           "  exec /etc/confluent/docker/run\n"
           "fi\n";
}

std::string kafka_topics_label(const std::vector<std::pair<std::string, int>>& topics) {
    std::string label;
    for (const auto& [name, partitions] : topics) {
        if (!label.empty()) {
            label += ',';
        }
        label += name;
        label += ':';
        label += std::to_string(partitions);
    }
    return label;
}

} // namespace testcontainers::modules::detail
