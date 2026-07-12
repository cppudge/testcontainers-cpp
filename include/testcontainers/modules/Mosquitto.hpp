#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testcontainers/Container.hpp"
#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/GenericImage.hpp"

namespace testcontainers::modules {

class MosquittoContainer;

/// A reusable, copyable description of an Eclipse Mosquitto MQTT broker: the
/// pinned image, port 1883 exposed, and a managed `mosquitto.conf` (copied in
/// before start) that accepts anonymous clients from outside the container —
/// the stock image config would listen on the container's loopback only, so
/// the mapped port would connect to nothing.
///
/// The `with_*` builders mutate in place and return `*this` by reference, so a
/// named config can be configured incrementally and started many times.
/// Core options the module does not surface are reached through
/// `with_customizer`; `to_generic()` drops down to a plain `GenericImage`
/// when a raw core `Container` is wanted instead.
class MosquittoImage {
public:
    /// The pinned default image. Override with `with_image`; the hub-prefix
    /// substitution (TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX) applies as usual.
    static constexpr std::string_view kDefaultImage = "eclipse-mosquitto:2.0";

    /// The MQTT port INSIDE the container. Peers on a shared docker network
    /// connect to `<alias-or-name>:kPort`; the test process itself uses
    /// `MosquittoContainer::port()` (the mapped host port) instead.
    static constexpr std::uint16_t kPort = 1883;

    /// Where the image reads its configuration — the managed config (or your
    /// `with_config` replacement) is copied here before start.
    static constexpr std::string_view kConfigPath = "/mosquitto/config/mosquitto.conf";

    /// A config ready to `start()`: image `eclipse-mosquitto:2.0`, port 1883
    /// exposed, managed config `listener 1883` + `allow_anonymous true`, and
    /// readiness = the broker's own "running" log line (printed after the
    /// listening sockets are open).
    MosquittoImage();

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Override the pinned image with a full reference "name[:tag]" (tag
    /// defaults to "latest"), e.g. "eclipse-mosquitto:2.1-alpine" or a mirror
    /// reference — any image that reads kConfigPath and logs startup to
    /// stdout/stderr works.
    MosquittoImage& with_image(const std::string& reference);

    /// Append one option line, `<key> <value>`, to the managed config — e.g.
    /// `("persistence", "true")`. Repeatable; lines land after the managed
    /// listener block in call order, so listener-scoped options bind to the
    /// module's port-1883 listener. Incompatible with `with_config` /
    /// `with_config_content` (render throws): a replaced config owns its
    /// options.
    MosquittoImage& with_config_option(std::string key, std::string value);

    /// Replace the managed config with a host file, copied to kConfigPath at
    /// start (the file is read at copy time). You then own the whole
    /// contract: keep a `listener 1883` (the published port — a listenerless
    /// config serves the container's loopback only while the readiness wait
    /// still passes), keep startup logs on stdout/stderr at the default
    /// log_type, and choose your own auth (`allow_anonymous` defaults to
    /// false once a listener is defined). Repeat calls replace again; the
    /// last one wins.
    MosquittoImage& with_config(std::filesystem::path host_conf);

    /// As `with_config`, from in-memory bytes instead of a host file.
    MosquittoImage& with_config_content(std::string conf);

    // --- Pass-throughs to the underlying builder ---

    /// Set an extra environment variable (for your own tooling — the image
    /// reads none itself; mosquitto is configured by file only, so there are
    /// no managed keys to collide with).
    MosquittoImage& with_env(std::string key, std::string value);

    MosquittoImage& with_label(std::string key, std::string value);

    /// Join a user-defined network; peers resolve this container by
    /// name/alias at `<alias>:1883` (kPort, not the mapped host port).
    MosquittoImage& with_network(std::string network);
    MosquittoImage& with_network(const Network& network);
    MosquittoImage& with_network_alias(std::string alias);

    /// Enable container reuse (effective only when reuse is also enabled
    /// globally — see GenericImage::with_reuse). An adopted broker keeps its
    /// retained messages and persistent sessions: same config, next run,
    /// state intact.
    MosquittoImage& with_reuse(bool reuse = true);

    /// Budget for the whole readiness phase (default: 60s; the broker itself
    /// boots in well under a second — the budget covers slow CI daemons).
    /// Image pull time does not count against it.
    MosquittoImage& with_startup_timeout(std::chrono::milliseconds timeout);

    /// Retry the whole create→start→wait sequence up to `n` times.
    MosquittoImage& with_startup_attempts(int n);

    /// Register a callback that customizes the underlying `GenericImage` —
    /// the channel for every option this module does not surface (extra
    /// listener ports, mounts, a password-file copy, ...). Customizers run
    /// when the config is rendered (`start()` / `to_generic()`), in
    /// registration order, AFTER the module's own rendering — what they set
    /// wins over the module.
    MosquittoImage& with_customizer(std::function<void(GenericImage&)> customize);

    // --- Getters ---

    /// Option lines accumulated by `with_config_option`, in call order.
    const std::vector<std::pair<std::string, std::string>>& config_options() const noexcept {
        return config_options_;
    }

    /// Render the full configuration — managed or replacement config file,
    /// option lines, customizers — into a plain GenericImage: the drop-down
    /// escape hatch when a raw core `Container` is wanted. Throws Error —
    /// before any daemon contact — when option lines are combined with a
    /// replacement config (see with_config_option).
    GenericImage to_generic() const;

    /// Create, start, and wait until the broker logs that it is running (the
    /// listening sockets are open by then). Throws on failure
    /// (`StartupTimeoutError` when the broker never becomes ready within the
    /// startup timeout), like `GenericImage::start()`.
    MosquittoContainer start() const;

private:
    GenericImage image_; ///< pin + port + wait + pass-through state
    std::vector<std::pair<std::string, std::string>> config_options_; ///< appended lines
    std::optional<CopyToContainer> user_config_;                  ///< replacement config, when set
    std::vector<std::function<void(GenericImage&)>> customizers_; ///< applied last
};

/// A running Mosquitto broker: connection getters plus the owned container.
///
/// Move-only — it owns the core `Container`, whose destructor force-removes
/// the broker (RAII teardown; `container().keep()` or reuse opt out, see
/// Container).
class MosquittoContainer {
public:
    /// The address a client in the test process connects to. Resolved once,
    /// when the container started.
    const std::string& host() const noexcept { return host_; }

    /// The host port published for the MQTT port 1883. Resolved once, when
    /// the container started; a container restarted by hand gets fresh
    /// ephemeral ports — re-resolve via `container()` then.
    std::uint16_t port() const noexcept { return port_; }

    /// The broker URL, e.g. `tcp://localhost:32768` — the serverURI form the
    /// Paho C/C++ clients take (they also accept the identical `mqtt://`
    /// spelling). Clients that take host and port directly (libmosquitto,
    /// mosquitto_pub/_sub) use `host()` / `port()` instead.
    const std::string& broker_url() const noexcept { return broker_url_; }

    /// The underlying container handle: exec `mosquitto_pub`/`mosquitto_sub`
    /// (both ship in the image), read logs, copy files, `stop()` early,
    /// `keep()` it past the test.
    Container& container() noexcept { return container_; }
    const Container& container() const noexcept { return container_; }

private:
    friend class MosquittoImage;
    explicit MosquittoContainer(Container container)
        : container_(std::move(container)), host_(container_.host()),
          port_(container_.get_host_port(tcp(MosquittoImage::kPort))),
          broker_url_("tcp://" + host_ + ":" + std::to_string(port_)) {}

    Container container_;
    std::string host_;       ///< resolved once at start()
    std::uint16_t port_ = 0; ///< resolved once at start()
    std::string broker_url_; ///< "tcp://<host>:<port>", assembled once
};

} // namespace testcontainers::modules
