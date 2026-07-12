#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testcontainers/Container.hpp"
#include "testcontainers/GenericImage.hpp"

namespace testcontainers::modules {

class StartedKafka;

/// A single-node Apache Kafka broker in KRaft mode (no ZooKeeper), for tests.
///
/// A copyable configuration that can be tuned incrementally and started many
/// times. `start()` runs the two-phase Kafka boot: the container first starts
/// with a placeholder command, then — once Docker has assigned the host port —
/// the module writes the real launch script (with the advertised listeners
/// pointing at the actual `host:port`) into the container and waits for the
/// broker to finish booting. The returned `StartedKafka` hands out the
/// addresses; no Kafka client library is required or linked.
///
/// Two client paths are preconfigured:
///  - host-side (your test process): `StartedKafka::bootstrap_servers()`;
///  - container-to-container: put the broker on a user-defined network with
///    `with_network` + `with_network_alias`, and point peer containers at
///    `StartedKafka::internal_bootstrap_servers()`.
///
/// Requires a Linux-containers daemon (the image is Linux-only).
class KafkaContainer {
public:
    /// The pinned default image — the official ASF image, purpose-built for
    /// single-node KRaft, shipping the CLI tools the module (and the
    /// exec-your-own-assertions style) relies on.
    static constexpr std::string_view kDefaultImage = "apache/kafka:3.9.1";

    /// The broker's in-container client port for host-side access. Published
    /// to an ephemeral host port; `StartedKafka::port()` is its mapping.
    static constexpr std::uint16_t kPort = 9092;

    /// The in-network listener port: peer containers on a shared user-defined
    /// network reach the broker at `<network alias>:kInternalPort`. Also the
    /// port for `exec` sessions inside the broker container itself
    /// (`localhost:9093`) — bootstrapping in-container clients against
    /// `localhost:9092` does not work, because the metadata handed back on
    /// that listener advertises the host-side address.
    static constexpr std::uint16_t kInternalPort = 9093;

    /// A config ready to `start()`: image `apache/kafka:3.9.1`, port 9092
    /// exposed, single-node KRaft with a fixed cluster id.
    KafkaContainer();

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Override the pinned image with a full reference "name[:tag]" — a
    /// mirror, another 3.x/4.x tag, or `apache/kafka-native` (note the native
    /// image ships no CLI tools, so `with_topic` and exec-based probing do
    /// not work there). The image must follow the apache/kafka docker
    /// conventions: `KAFKA_*` env overrides and a `/etc/kafka/docker/run` or
    /// `/etc/confluent/docker/run` launch script (the confluent fallback
    /// covers the BOOT only — `with_topic` still execs the apache CLI path).
    KafkaContainer& with_image(const std::string& reference);

    /// Add an environment variable for the broker, e.g.
    /// `with_env("KAFKA_AUTO_CREATE_TOPICS_ENABLE", "false")` or
    /// `with_env("KAFKA_NUM_PARTITIONS", "3")`. Applied after the module's
    /// own settings, so on a duplicate key yours wins — overriding the
    /// module's listener/KRaft plumbing (KAFKA_LISTENERS,
    /// KAFKA_PROCESS_ROLES, ...) can break startup, and the CLUSTER_ID key
    /// belongs to `with_cluster_id` (overriding it here desyncs
    /// `StartedKafka::cluster_id()`).
    KafkaContainer& with_env(std::string key, std::string value) {
        extra_env_.emplace_back(std::move(key), std::move(value));
        return *this;
    }

    /// Attach a metadata label. The module's reuse-visibility label
    /// (`org.testcontainers.kafka.topics`) is applied after these, so it
    /// wins on a duplicate key.
    KafkaContainer& with_label(std::string key, std::string value) {
        image_.with_label(std::move(key), std::move(value));
        return *this;
    }

    /// Set the KRaft cluster id: 22 characters of URL-safe base64 (a 16-byte
    /// UUID, e.g. from `kafka-storage.sh random-uuid`). Defaults to a fixed
    /// module-wide id, which keeps restarts and container reuse
    /// deterministic. A malformed id makes `start()` throw up front — the
    /// broker's own failure mode is an opaque storage-format error followed
    /// by the full startup timeout.
    KafkaContainer& with_cluster_id(std::string cluster_id) {
        cluster_id_ = std::move(cluster_id);
        return *this;
    }

    /// Join a user-defined network; with an alias this enables
    /// `internal_bootstrap_servers()` for peer containers.
    KafkaContainer& with_network(std::string network) {
        image_.with_network(std::move(network));
        return *this;
    }
    KafkaContainer& with_network(const Network& network);

    /// DNS alias for the broker on its network. The FIRST alias becomes the
    /// advertised host of the internal listener (`<alias>:9093`). Requires
    /// `with_network`. Without an alias, peers on the same user-defined
    /// network can still use the short-container-id form the getters return.
    KafkaContainer& with_network_alias(std::string alias) {
        image_.with_network_alias(std::move(alias));
        return *this;
    }

    /// Create a topic right after the broker becomes ready (replication
    /// factor 1; `--if-not-exists`). Add several for several topics. Only
    /// needed for multi-partition topics or when auto-topic-creation is
    /// disabled — by default the broker auto-creates single-partition topics
    /// on first use. Not supported on `apache/kafka-native` (no CLI tools in
    /// the image). A failed creation fails `start()`.
    KafkaContainer& with_topic(std::string name, int partitions = 1) {
        topics_.emplace_back(std::move(name), partitions);
        return *this;
    }

    /// Budget for EACH of the two startup phases (placeholder up; broker
    /// ready after reconfiguration), so the worst-case total is about twice
    /// this. Default 60s per phase. Image pull time is not counted.
    KafkaContainer& with_startup_timeout(std::chrono::milliseconds timeout) {
        image_.with_startup_timeout(timeout);
        return *this;
    }

    /// Retry the whole create→configure→wait sequence up to `n` times (a
    /// fresh container per attempt); a failure in the boot choreography
    /// participates like any startup failure.
    KafkaContainer& with_startup_attempts(int n) {
        image_.with_startup_attempts(n);
        return *this;
    }

    /// Enable container reuse (effective only when reuse is enabled globally;
    /// see GenericImage::with_reuse). An adopted broker keeps the
    /// configuration of its original start — its advertised listeners still
    /// match its unchanged port binding; the `with_topic` list participates
    /// in the reuse match, so changing it creates a fresh container.
    KafkaContainer& with_reuse(bool reuse = true) {
        image_.with_reuse(reuse);
        return *this;
    }

    /// Register a callback that customizes the underlying `GenericImage` —
    /// the channel for options this module does not surface (pull policy,
    /// mounts, ...). Customizers run when the config is rendered
    /// (`start()` / `to_generic()`), in registration order, AFTER the
    /// module's own rendering. Caveats: the boot choreography lives in the
    /// rendered command, wait, and started hook — replacing any of them
    /// breaks the boot — and the hook has already captured this config's
    /// network alias, topics, and timeout, so set those through the module's
    /// setters, not here.
    KafkaContainer& with_customizer(std::function<void(GenericImage&)> customize) {
        customizers_.push_back(std::move(customize));
        return *this;
    }

    // --- Getters ---

    /// The configured KRaft cluster id.
    const std::string& cluster_id() const noexcept { return cluster_id_; }

    /// Render the fully assembled underlying builder — image, env,
    /// placeholder command, sentinel wait, and the started hook that
    /// performs the Kafka boot choreography. The escape hatch for starting
    /// on your own terms: tune the result, `start()` it yourself, and derive
    /// addresses from the returned Container
    /// (`host()` + `get_host_port(tcp(KafkaContainer::kPort))`). The
    /// `with_customizer` caveats apply here too. Throws Error on an invalid
    /// config (malformed cluster id) before any daemon contact.
    GenericImage to_generic() const;

    /// Create, start, reconfigure, and wait for the broker; returns the
    /// running handle. Throws on failure (StartupTimeoutError when the
    /// broker does not become ready in time — with the last log lines in the
    /// message; DockerError for daemon failures), with the partial container
    /// cleaned up.
    StartedKafka start() const;

private:
    GenericImage image_; ///< pin + port + network/reuse/timeout pass-through state
    std::string cluster_id_{"4L6g3nShT-eMCtK--X86sw"}; ///< fixed default: deterministic reuse
    std::vector<std::pair<std::string, std::string>> extra_env_;  ///< after module env
    std::vector<std::pair<std::string, int>> topics_;             ///< name -> partitions
    std::vector<std::function<void(GenericImage&)>> customizers_; ///< applied last
};

/// A running single-node Kafka broker: address getters plus the owned
/// container.
///
/// Move-only — it owns the `Container`, whose destructor force-removes the
/// broker (RAII teardown; `container().keep()` or reuse opt out, see
/// Container). The address getters are resolved once, at start, and stay
/// valid for the container's lifetime.
class StartedKafka {
public:
    /// The `bootstrap.servers` value for clients in THIS process (librdkafka,
    /// kcat, ...): `"<host>:<mapped port>"`, e.g. "localhost:32771". Bare
    /// host:port — no `PLAINTEXT://` scheme (librdkafka rejects one).
    const std::string& bootstrap_servers() const noexcept { return bootstrap_; }

    /// The `bootstrap.servers` value for OTHER CONTAINERS sharing the
    /// broker's user-defined network: `"<first network alias>:9093"` (or
    /// `"<short container id>:9093"` when no alias was configured —
    /// resolvable on user-defined networks, but not on the default bridge).
    /// Configure `with_network` + `with_network_alias` for a stable,
    /// readable value.
    const std::string& internal_bootstrap_servers() const noexcept { return internal_bootstrap_; }

    /// Host and mapped port of the client listener, for callers assembling
    /// their own config; `bootstrap_servers()` is `host():port()`.
    const std::string& host() const noexcept { return host_; }
    std::uint16_t port() const noexcept { return port_; }

    /// The KRaft cluster id the broker was formatted with.
    const std::string& cluster_id() const noexcept { return cluster_id_; }

    /// The underlying container handle: exec the in-image CLI tools
    /// (bootstrap them at `localhost:9093` — see kInternalPort), read logs,
    /// `stop()` early, `keep()` it past the test.
    Container& container() noexcept { return container_; }
    const Container& container() const noexcept { return container_; }

private:
    friend class KafkaContainer;
    StartedKafka(Container container, std::string internal_host, std::string cluster_id)
        : container_(std::move(container)), host_(container_.host()),
          port_(container_.get_host_port(tcp(KafkaContainer::kPort))),
          bootstrap_(host_ + ":" + std::to_string(port_)),
          internal_bootstrap_(std::move(internal_host) + ":" +
                              std::to_string(KafkaContainer::kInternalPort)),
          cluster_id_(std::move(cluster_id)) {}

    Container container_;
    std::string host_;               ///< resolved once at start()
    std::uint16_t port_ = 0;         ///< resolved once at start()
    std::string bootstrap_;          ///< "<host>:<port>", assembled once
    std::string internal_bootstrap_; ///< "<alias|short-id>:9093"
    std::string cluster_id_;
};

} // namespace testcontainers::modules
