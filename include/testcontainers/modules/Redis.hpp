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

class RedisContainer;

/// A reusable, copyable description of a Redis server container: the pinned
/// image, the exposed server port, an in-container `redis-cli ping` readiness
/// probe, and optional password / extra server arguments.
///
/// The `with_*` builders mutate in place and return `*this` by reference, so a
/// named config can be configured incrementally and started many times.
/// Core options the module does not surface are reached through
/// `with_customizer`; `to_generic()` drops down to a plain `GenericImage`
/// when a raw core `Container` is wanted instead.
class RedisImage {
public:
    /// The pinned default image. Override with `with_image`; the hub-prefix
    /// substitution (TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX) applies as usual.
    static constexpr std::string_view kDefaultImage = "redis:7.2";

    /// The server port INSIDE the container. Peers on a shared docker network
    /// connect to `<alias-or-name>:kPort`; the test process itself uses
    /// `RedisContainer::port()` (the mapped host port) instead.
    static constexpr std::uint16_t kPort = 6379;

    /// A config ready to `start()`: image `redis:7.2`, port 6379 exposed, and
    /// readiness = an in-container `redis-cli ping` answering successfully.
    RedisImage();

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Override the pinned image with a full reference "name[:tag]" (tag
    /// defaults to "latest"), e.g. "redis:7.4" or a registry-qualified mirror
    /// reference. The image must ship `redis-cli` — the readiness probe runs
    /// it; every official `redis` tag does.
    RedisImage& with_image(const std::string& reference);

    /// Require a password: the server starts with `--requirepass <password>`
    /// and `RedisContainer::connection_string()` gains `:password@`. The
    /// container also carries `REDISCLI_AUTH=<password>`, so in-container
    /// `redis-cli` runs — the readiness probe and any `exec` you issue —
    /// authenticate automatically. An empty password means no auth (the
    /// default). Test credentials only: the value is visible in the
    /// container's command line and environment via inspect.
    RedisImage& with_password(std::string password);

    /// Append arguments to the `redis-server` command line, e.g.
    /// `{"--maxmemory", "64mb"}`. Repeatable; calls accumulate in order,
    /// placed after `--requirepass` when a password is set. Setting any args
    /// (or a password) makes the module own the container command; a
    /// customizer's `with_cmd` still wins (customizers run last).
    RedisImage& with_command_args(std::vector<std::string> args);

    /// The single-argument twin of `with_command_args` (same placement and
    /// accumulation rules).
    RedisImage& with_command_arg(std::string arg);

    // --- Pass-throughs to the underlying builder ---

    /// Set an extra environment variable. REDISCLI_AUTH belongs to
    /// with_password: setting it here alongside a password makes `start()`
    /// throw up front — unlike the DB modules' bash-read credential keys, it
    /// is read by EXEC'D processes (the readiness probe's redis-cli), where
    /// the FIRST duplicate of a key wins, so the module could not override a
    /// raw entry by ordering. Without a password the key is yours to set
    /// (custom auth setups).
    RedisImage& with_env(std::string key, std::string value);

    RedisImage& with_label(std::string key, std::string value);

    /// Join a user-defined network; peers resolve this container by
    /// name/alias at `<alias>:6379` (kPort, not the mapped host port).
    RedisImage& with_network(std::string network);
    RedisImage& with_network(const Network& network);
    RedisImage& with_network_alias(std::string alias);

    /// Enable container reuse (effective only when reuse is also enabled
    /// globally — see GenericImage::with_reuse). An adopted server keeps its
    /// keyspace: same config, next run, data intact.
    RedisImage& with_reuse(bool reuse = true);

    /// Budget for the whole readiness phase (default: 60s). Image pull time
    /// does not count against it.
    RedisImage& with_startup_timeout(std::chrono::milliseconds timeout);

    /// Retry the whole create→start→wait sequence up to `n` times.
    RedisImage& with_startup_attempts(int n);

    /// Register a callback that customizes the underlying `GenericImage` —
    /// the channel for every option this module does not surface (mounts,
    /// pull policy, ...). Customizers run when the config is rendered
    /// (`start()` / `to_generic()`), in registration order, AFTER the
    /// module's own rendering — what they set wins over the module. Do not
    /// set REDISCLI_AUTH here: with_password manages it (see with_env).
    RedisImage& with_customizer(std::function<void(GenericImage&)> customize);

    // --- Getters ---

    /// The configured password; empty when none.
    const std::string& password() const noexcept { return password_; }

    /// Extra `redis-server` arguments accumulated so far.
    const std::vector<std::string>& command_args() const noexcept { return args_; }

    /// Render the full configuration — module options and customizers applied
    /// — into a plain GenericImage: the drop-down escape hatch when you need
    /// a raw core `Container` (or run-level tweaks) instead of a RedisContainer.
    /// Throws Error — before any daemon contact — when `with_env` carries
    /// REDISCLI_AUTH alongside a configured password (see with_env).
    GenericImage to_generic() const;

    /// Create, start, and wait until the server answers `redis-cli ping`.
    /// Throws on failure (`StartupTimeoutError` when the server never becomes
    /// ready within the startup timeout), like `GenericImage::start()`.
    RedisContainer start() const;

private:
    GenericImage image_;            ///< pin + port + ping wait + pass-through state
    std::string password_;          ///< rendered into cmd/env when set
    std::vector<std::string> args_; ///< rendered into cmd when non-empty
    std::vector<std::function<void(GenericImage&)>> customizers_; ///< applied last
};

/// A running Redis server: connection getters plus the owned container.
///
/// Move-only — it owns the core `Container`, whose destructor force-removes the
/// server (RAII teardown; `container().keep()` or reuse opt out, see
/// Container).
class RedisContainer {
public:
    /// The address a client in the test process connects to. Resolved once,
    /// when the container started.
    const std::string& host() const noexcept { return host_; }

    /// The host port published for the server port 6379. Resolved once, when
    /// the container started; a container restarted by hand gets fresh
    /// ephemeral ports — re-resolve via `container()` then.
    std::uint16_t port() const noexcept { return port_; }

    /// The password the server requires; empty when none.
    const std::string& password() const noexcept { return password_; }

    /// The connection URL, `redis://[:password@]host:port[/database]` — for
    /// DSN-taking clients and `redis-cli -u`. `database` is the numeric Redis
    /// database index; 0 (the default) emits no path segment. The password is
    /// percent-encoded. Clients that take host and port directly (hiredis,
    /// Boost.Redis, redis-plus-plus) use `host()` / `port()` instead.
    std::string connection_string(int database = 0) const;

    /// The underlying container handle: exec `redis-cli`, read logs, copy
    /// files, `stop()` early, `keep()` it past the test.
    Container& container() noexcept { return container_; }
    const Container& container() const noexcept { return container_; }

private:
    friend class RedisImage;
    RedisContainer(Container container, std::string password)
        : container_(std::move(container)), password_(std::move(password)),
          host_(container_.host()), port_(container_.get_host_port(tcp(RedisImage::kPort))) {}

    Container container_;
    std::string password_;
    std::string host_;       ///< resolved once at start()
    std::uint16_t port_ = 0; ///< resolved once at start()
};

} // namespace testcontainers::modules
