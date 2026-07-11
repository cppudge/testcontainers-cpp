#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testcontainers/Container.hpp"
#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/GenericImage.hpp"

namespace testcontainers::modules {

class StartedRabbitMQ;

/// A reusable, copyable description of a RabbitMQ broker: image, credentials,
/// virtual host, preloaded definitions, and plugins to enable.
///
/// The `with_*` builders mutate in place and return `*this` by reference, so a
/// named config can be configured incrementally and started many times. The
/// default image is the `-management` variant: the same broker with the
/// management plugin enabled, serving the HTTP API and UI on 15672 — the
/// standard way for a test to inspect queues, connections, and messages
/// without an AMQP client library.
///
/// Readiness is deliberately ORDERED: the "Server startup complete" log line
/// first, one `rabbitmq-diagnostics check_port_connectivity` exec second. An
/// exec-based probe that fires in the first seconds of boot can permanently
/// prevent the broker from starting (a root-owned Erlang cookie the server's
/// own user then cannot read) — keep the module's waits first if you add more
/// through `with_customizer`.
class RabbitMQContainer {
public:
    /// The pinned default image. The management HTTP API (15672) is enabled;
    /// AMQP listens on 5672. Getters tied to the management API assume a
    /// `-management` variant. Note on custom/hardened images: the official
    /// image ships `loopback_users.guest = false` — without that line the
    /// built-in guest account only works over loopback, so use
    /// `with_username` (custom users are never loopback-restricted).
    static constexpr std::string_view kDefaultImage = "rabbitmq:3.13-management";

    /// The AMQP 0-9-1 port INSIDE the container. Peers on a shared docker
    /// network connect to `<alias-or-name>:kAmqpPort`; the test process uses
    /// `StartedRabbitMQ::amqp_port()` (the mapped host port) instead.
    static constexpr std::uint16_t kAmqpPort = 5672;

    /// The management HTTP API/UI port INSIDE the container.
    static constexpr std::uint16_t kManagementPort = 15672;

    /// A config ready to `start()`: image `rabbitmq:3.13-management`, ports
    /// 5672 + 15672 exposed, credentials guest/guest on vhost "/", ordered
    /// readiness (log line, then one diagnostics exec).
    RabbitMQContainer();

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Override the pinned image with a full reference "name[:tag]" (tag
    /// defaults to "latest") — e.g. a mirror or a `-management` tag of a
    /// different line.
    RabbitMQContainer& with_image(const std::string& reference);

    /// The broker's provisioned user (`RABBITMQ_DEFAULT_USER`). Default
    /// "guest". The configured user REPLACES the built-in guest account —
    /// after `with_username("app")` there is no "guest" user on the broker.
    RabbitMQContainer& with_username(std::string username) {
        username_ = std::move(username);
        return *this;
    }

    /// The provisioned user's password (`RABBITMQ_DEFAULT_PASS`). Default
    /// "guest". Must not be empty — the broker's internal auth backend
    /// prohibits blank-password logins, so `start()` throws up front. Test
    /// credentials only: the value is visible via inspect.
    RabbitMQContainer& with_password(std::string password) {
        password_ = std::move(password);
        return *this;
    }

    /// The default virtual host (`RABBITMQ_DEFAULT_VHOST`). Default "/" (the
    /// broker's standard default vhost). The configured user gets full
    /// permissions on it. Any UTF-8 name is accepted; `amqp_url()` renders it
    /// percent-encoded as one path segment.
    RabbitMQContainer& with_vhost(std::string vhost) {
        vhost_ = std::move(vhost);
        return *this;
    }

    /// Import a definitions JSON file (the broker's export format: exchanges,
    /// queues, bindings, policies, users, ...) at boot — the standard way to
    /// preload topology. Repeatable; files import in call order, later files
    /// winning on conflicting objects. The file name must end in ".json"
    /// (anything else throws here).
    ///
    /// The configured user/vhost are seeded ALONGSIDE your file, so it only
    /// needs the application objects: RabbitMQ skips ALL default-account
    /// provisioning when definitions are imported at boot, and without the
    /// seeding a definitions file with no "users" entry would leave the
    /// broker with no users at all. Declaring the same user/vhost in your
    /// file overrides the seeded one (your file imports later). Objects must
    /// reference vhosts that exist after import — the seeded vhost is
    /// `vhost()`; declare any other one your objects live in.
    RabbitMQContainer& with_definitions(std::filesystem::path host_json);

    /// As `with_definitions`, from in-memory JSON bytes instead of a host file.
    RabbitMQContainer& with_definitions_json(std::string json);

    /// Enable a plugin that ships with the image (e.g. "rabbitmq_shovel",
    /// "rabbitmq_federation") once the broker is ready, via
    /// `rabbitmq-plugins enable` — additive, so the image's own enabled
    /// plugins (management, prometheus) are kept. Repeatable; all names are
    /// enabled in one command. Plugins that add listeners on new ports (MQTT,
    /// STOMP) are enabled but their ports are not published — expose them
    /// through `with_customizer` if you need them mapped. A failed enable
    /// (e.g. a typo'd name) fails `start()`.
    RabbitMQContainer& with_plugin(std::string plugin) {
        plugins_.push_back(std::move(plugin));
        return *this;
    }

    // --- Pass-throughs to the underlying builder ---

    /// Join a user-defined network; peers resolve this container by
    /// name/alias at `<alias>:5672` (kAmqpPort, not the mapped host port).
    RabbitMQContainer& with_network(std::string network) {
        image_.with_network(std::move(network));
        return *this;
    }
    RabbitMQContainer& with_network(const Network& network);
    RabbitMQContainer& with_network_alias(std::string alias) {
        image_.with_network_alias(std::move(alias));
        return *this;
    }

    /// Enable container reuse (effective only when reuse is also enabled
    /// globally — see GenericImage::with_reuse). An adopted broker keeps its
    /// definitions and runtime-enabled plugins.
    RabbitMQContainer& with_reuse(bool reuse = true) {
        image_.with_reuse(reuse);
        return *this;
    }

    /// Budget for the whole readiness phase (default: 60s; observed cold
    /// boots take ~10s). Image pull time does not count against it.
    RabbitMQContainer& with_startup_timeout(std::chrono::milliseconds timeout) {
        image_.with_startup_timeout(timeout);
        return *this;
    }

    /// Retry the whole create→start→wait sequence up to `n` times.
    RabbitMQContainer& with_startup_attempts(int n) {
        image_.with_startup_attempts(n);
        return *this;
    }

    /// Register a callback that customizes the underlying `GenericImage` —
    /// the channel for every option this module does not surface (extra
    /// ports, labels, pull policy, ...). Customizers run when the config is
    /// rendered (`start()` / `to_generic()`), in registration order, AFTER
    /// the module's own rendering — what they set wins over the module. A
    /// wait added here runs after the module's ordered waits, which is the
    /// SAFE position (see the class note on the Erlang cookie); do not
    /// replace the module's waits. Do not set the RABBITMQ_DEFAULT_* env
    /// here either: it would desync the credential getters and `amqp_url()`
    /// — use the typed setters instead.
    RabbitMQContainer& with_customizer(std::function<void(GenericImage&)> customize) {
        customizers_.push_back(std::move(customize));
        return *this;
    }

    // --- Getters ---

    const std::string& username() const noexcept { return username_; }
    const std::string& password() const noexcept { return password_; }
    const std::string& vhost() const noexcept { return vhost_; }

    /// Render the full configuration — env trio, definitions plumbing
    /// (seeded account + your files + the load_definitions drop-in), ordered
    /// waits, plugin hook, customizers — into a plain GenericImage: the
    /// drop-down escape hatch when you need a raw `Container` instead of a
    /// StartedRabbitMQ. Throws Error on an invalid config (empty
    /// username/vhost) before any daemon contact.
    GenericImage to_generic() const;

    /// Create, start, and wait for the broker (the ordered readiness above).
    /// Throws on failure (`StartupTimeoutError` when the broker never becomes
    /// ready, DockerError for daemon failures), like `GenericImage::start()`.
    StartedRabbitMQ start() const;

private:
    /// The next user-definitions target: /etc/rabbitmq/definitions.d/05NN-…
    std::string next_definitions_target() const;

    GenericImage image_;            ///< pin + ports + pass-through state
    std::string username_{"guest"}; ///< RABBITMQ_DEFAULT_USER, rendered at start()
    std::string password_{"guest"}; ///< RABBITMQ_DEFAULT_PASS
    std::string vhost_{"/"};        ///< RABBITMQ_DEFAULT_VHOST
    /// User definition files, already shaped as copies targeting
    /// /etc/rabbitmq/definitions.d/05NN-definitions.json (NN = call order,
    /// after the module's 0010- seed file so user files win on conflicts).
    std::vector<CopyToContainer> definitions_;
    std::vector<std::string> plugins_;
    std::vector<std::function<void(GenericImage&)>> customizers_; ///< applied last
};

/// A running RabbitMQ broker: connection getters plus the owned container.
///
/// Move-only — it owns the `Container`, whose destructor force-removes the
/// broker (RAII teardown; `container().keep()` or reuse opt out, see
/// Container).
class StartedRabbitMQ {
public:
    /// The address a client in the test process connects to. Resolved once,
    /// when the container started.
    const std::string& host() const noexcept { return host_; }

    /// The host port published for AMQP 0-9-1 (container port 5672).
    /// Resolved once, when the container started.
    std::uint16_t amqp_port() const noexcept { return amqp_port_; }

    /// The host port published for the management HTTP API/UI (container
    /// port 15672). Meaningful only for a `-management` image variant; with
    /// a plain image the port is published but nothing listens on it.
    std::uint16_t management_port() const noexcept { return management_port_; }

    const std::string& username() const noexcept { return username_; }
    const std::string& password() const noexcept { return password_; }
    const std::string& vhost() const noexcept { return vhost_; }

    /// The AMQP connection URI, e.g. `amqp://guest:guest@localhost:32771`.
    ///
    /// For the default vhost "/" no path segment is emitted: in an AMQP URI
    /// an absent path means the client's default vhost ("/" in every
    /// mainstream client), and unlike the equivalent explicit `/%2F`
    /// spelling it also survives URI parsers that skip percent-decoding. Any
    /// other vhost is appended percent-encoded as one path segment
    /// ("/orders", "/a%2Fb"). Clients taking discrete arguments instead of a
    /// URI (rabbitmq-c's amqp_login, SimpleAmqpClient) use `host()` /
    /// `amqp_port()` / `username()` / `password()` / `vhost()` directly.
    std::string amqp_url() const;

    /// The management base URL, e.g. `http://localhost:32772`. REST
    /// endpoints live under `/api/...` and take HTTP basic auth with
    /// `username()` / `password()`.
    std::string management_url() const;

    /// The underlying container handle: exec `rabbitmqctl` /
    /// `rabbitmq-diagnostics`, read logs, `stop()` early, `keep()` it past
    /// the test.
    Container& container() noexcept { return container_; }
    const Container& container() const noexcept { return container_; }

private:
    friend class RabbitMQContainer;
    StartedRabbitMQ(Container container, std::string username, std::string password,
                    std::string vhost)
        : container_(std::move(container)), username_(std::move(username)),
          password_(std::move(password)), vhost_(std::move(vhost)), host_(container_.host()),
          amqp_port_(container_.get_host_port(tcp(RabbitMQContainer::kAmqpPort))),
          management_port_(container_.get_host_port(tcp(RabbitMQContainer::kManagementPort))) {}

    Container container_;
    std::string username_;
    std::string password_;
    std::string vhost_;
    std::string host_;                  ///< resolved once at start()
    std::uint16_t amqp_port_ = 0;       ///< resolved once at start()
    std::uint16_t management_port_ = 0; ///< resolved once at start()
};

} // namespace testcontainers::modules
