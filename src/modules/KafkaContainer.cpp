#include "testcontainers/modules/KafkaContainer.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "KafkaDetail.hpp"
#include "WaitStrategies.hpp"
#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers::modules {

namespace {

/// Phase 2 of the boot, run as the started hook (i.e. after the placeholder's
/// sentinel wait passed): resolve the mapped port, write the starter script —
/// so the broker's FIRST boot already advertises the right addresses — then
/// follow the logs until the KRaft server reports started, and pre-create the
/// requested topics. Throwing here aborts start() with cleanup and
/// participates in with_startup_attempts retries, like any startup failure.
void run_kafka_boot(DockerClient& client, const std::string& id,
                    const std::optional<std::string>& alias,
                    const std::vector<std::pair<std::string, int>>& topics,
                    std::chrono::milliseconds phase_budget) {
    // The non-owning handle: host() resolves through the same
    // override/gateway-aware path StartedKafka uses later — the advertised
    // host and the host handed to the user must be the same string, or
    // clients would bootstrap fine and then dial a dead address from the
    // metadata.
    Container probe = Container::adopt(client, id, AdoptOwnership::Keep);
    const std::string advertised_host = probe.host();
    const std::uint16_t mapped_port = probe.get_host_port(tcp(KafkaContainer::kPort));
    const std::string internal_host = alias ? *alias : id.substr(0, 12);

    probe.copy_to(CopyToContainer::content(
                      detail::kafka_starter_script(advertised_host, mapped_port, internal_host),
                      std::string(detail::kKafkaStarterPath))
                      .with_mode(0755));

    // A fresh budget for this phase: the broker only starts booting now that
    // the placeholder loop sees the script.
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + phase_budget;
    // The core's chunk-boundary-safe substring scanners (modules::detail is
    // the module helpers' namespace, hence the full qualification). Per
    // source, like the core log wait: an interleaved other-stream frame must
    // not split a match; the line counts on either stream.
    testcontainers::detail::OccurrenceCounter seen_on_stdout{std::string(detail::kKafkaReadyLine)};
    testcontainers::detail::OccurrenceCounter seen_on_stderr{std::string(detail::kKafkaReadyLine)};
    const FollowEnd end = client.follow_logs(
        id, LogOptions{}, // tail="all": a boot faster than the attach is still seen
        [&seen_on_stdout, &seen_on_stderr](LogSource source, std::string_view chunk) {
            (source == LogSource::Stdout ? seen_on_stdout : seen_on_stderr).feed(chunk);
            // Stop following once the line appeared on either stream.
            return seen_on_stdout.count() + seen_on_stderr.count() == 0;
        },
        deadline);
    if (end != FollowEnd::ConsumerStopped) {
        // DeadlineExpired, or StreamEnded = the container died mid-boot. Put
        // the actual broker error (bad env, storage-format failure, OOM) into
        // the message instead of a bare "timed out".
        LogOptions tail_opts;
        tail_opts.tail = "50";
        std::string log_tail;
        try {
            const ContainerLogs logs = client.logs(id, tail_opts);
            log_tail = logs.stderr_data.empty() ? logs.stdout_data : logs.stderr_data;
        } catch (const DockerError&) {
            log_tail = "<logs unavailable>";
        }
        if (log_tail.size() > 2048) {
            log_tail.erase(0, log_tail.size() - 2048);
        }
        throw StartupTimeoutError(std::string(end == FollowEnd::StreamEnded
                                                  ? "kafka broker exited during startup"
                                                  : "kafka broker did not become ready in time") +
                                      "; last log lines: " + log_tail,
                                  id);
    }

    const std::string internal_bootstrap =
        "localhost:" + std::to_string(KafkaContainer::kInternalPort);
    for (const auto& [name, partitions] : topics) {
        // The INTERNAL listener on purpose: metadata answered on :9092
        // advertises the host-side address, unreachable from inside the
        // container. No stdin, no TTY — works on every transport.
        const ExecResult res = client.exec(
            id, {"/opt/kafka/bin/kafka-topics.sh", "--bootstrap-server", internal_bootstrap,
                 "--create", "--if-not-exists", "--topic", name, "--partitions",
                 std::to_string(partitions), "--replication-factor", "1"});
        if (res.exit_code != 0) {
            throw DockerError("failed to create kafka topic \"" + name + "\" (exit " +
                                  std::to_string(res.exit_code) + "): " +
                                  (res.stderr_data.empty() ? res.stdout_data : res.stderr_data),
                              std::nullopt, id);
        }
    }
}

} // namespace

KafkaContainer::KafkaContainer()
    : image_(GenericImage::from_reference(std::string(kDefaultImage))) {
    // Only the client port is published; the internal and controller
    // listeners stay docker-network-local.
    image_.with_exposed_port(tcp(kPort));
}

KafkaContainer& KafkaContainer::with_image(const std::string& reference) {
    image_.with_image(reference);
    return *this;
}

// Out of line so the header needs no Network definition.
KafkaContainer& KafkaContainer::with_network(const Network& network) {
    image_.with_network(network);
    return *this;
}

GenericImage KafkaContainer::to_generic() const {
    if (!detail::valid_kafka_cluster_id(cluster_id_)) {
        // Fail fast: the broker's own failure mode is an opaque
        // storage-format error followed by the full startup timeout.
        throw Error("KRaft cluster id \"" + cluster_id_ +
                    "\" must be exactly 22 URL-safe base64 characters "
                    "(kafka-storage.sh random-uuid prints one)");
    }
    for (const auto& [name, partitions] : topics_) {
        if (name.empty() || partitions < 1) {
            // Fail fast instead of an opaque topic-creation error at start().
            throw Error("kafka topic \"" + name +
                        "\" needs a non-empty name and at least one partition");
        }
    }

    // Render into a COPY: repeated to_generic()/start() calls must never
    // accumulate state in the config.
    GenericImage generic = image_;

    // Module env first, user env after: the image's launch script is bash,
    // so on a duplicate key the user's (later) entry wins — Kafka's env is
    // broker tuning, not handle-mirrored credentials, so the user having the
    // last word is the useful order here.
    for (const auto& [key, value] : detail::kafka_env(cluster_id_)) {
        generic.with_env(key, value);
    }
    for (const auto& [key, value] : extra_env_) {
        generic.with_env(key, value);
    }

    // The placeholder: the broker must NOT boot yet — its advertised
    // listeners need the mapped port, which exists only after start. The
    // request-level wait gates on the placeholder's sentinel (proves the
    // container process runs and the log pipeline flows); the started hook
    // does the rest.
    generic.with_cmd({"sh", "-c", detail::kafka_await_command()});
    generic.with_wait(wait_for::log(std::string(detail::kKafkaSentinel)));

    if (!topics_.empty()) {
        // The topic list lives inside the hook lambda, invisible to the
        // reuse hash — this label pushes it into the create body, so a
        // changed topic set creates a fresh container instead of silently
        // adopting one without the new topics.
        generic.with_label(std::string(detail::kKafkaTopicsLabel),
                           detail::kafka_topics_label(topics_));
    }

    const std::optional<std::string> alias =
        image_.network_aliases().empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>(image_.network_aliases().front());
    const std::vector<std::pair<std::string, int>> topics = topics_;
    const std::chrono::milliseconds phase_budget = image_.startup_timeout();
    generic.with_started_hook(
        [alias, topics, phase_budget](DockerClient& client, const std::string& id) {
            run_kafka_boot(client, id, alias, topics, phase_budget);
        });

    // Customizers last: what the escape hatch sets wins over the module's
    // rendering (the header documents which parts must not be replaced).
    for (const auto& customize : customizers_) {
        customize(generic);
    }
    return generic;
}

StartedKafka KafkaContainer::start() const {
    Container container = to_generic().start();
    // The same authority the hook advertised: first alias, else short id.
    std::string internal_host = image_.network_aliases().empty() ? container.id().substr(0, 12)
                                                                 : image_.network_aliases().front();
    return StartedKafka(std::move(container), std::move(internal_host), cluster_id_);
}

} // namespace testcontainers::modules
