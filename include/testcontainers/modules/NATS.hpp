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

class NATSContainer;

/// A reusable, copyable description of a NATS server container: the pinned
/// image, the client and HTTP-monitoring ports, a log + `/healthz` readiness
/// pair, and optional credentials / JetStream / extra server flags — all
/// rendered into the command line (the server reads no environment).
///
/// The `with_*` builders mutate in place and return `*this` by reference, so a
/// named config can be configured incrementally and started many times.
/// Core options the module does not surface are reached through
/// `with_customizer`; `to_generic()` drops down to a plain `GenericImage`
/// when a raw core `Container` is wanted instead.
class NATSImage {
public:
    /// The pinned default image. Built FROM scratch: the only file inside is
    /// the server binary, so `container().exec(...)` has nothing to run —
    /// assert from the host instead (raw TCP on `port()`, or the monitoring
    /// API on `monitoring_port()`). The `-alpine` tags add a busybox shell;
    /// the module's flags-only command works on both variants. Override with
    /// `with_image`; the hub-prefix substitution
    /// (TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX) applies as usual.
    static constexpr std::string_view kDefaultImage = "nats:2.12";

    /// The client port INSIDE the container. Peers on a shared docker network
    /// connect to `<alias-or-name>:kClientPort`; the test process itself uses
    /// `NATSContainer::port()` (the mapped host port) instead.
    static constexpr std::uint16_t kClientPort = 4222;

    /// The HTTP monitoring port INSIDE the container (`/healthz`, `/varz`,
    /// `/connz`, `/jsz`, ...). Always enabled by the module (`-m 8222`);
    /// the endpoints answer plain unauthenticated GETs.
    static constexpr std::uint16_t kMonitoringPort = 8222;

    /// A config ready to `start()`: image `nats:2.12`, ports 4222 + 8222
    /// exposed, no auth, JetStream off; readiness = the "Server is ready" log
    /// line, then `/healthz` answering 200 through the published port.
    NATSImage();

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Override the pinned image with a full reference "name[:tag]" (tag
    /// defaults to "latest"), e.g. "nats:2.14-alpine" or a registry-qualified
    /// mirror reference. The image must accept `nats-server` flags as its
    /// container command; every official `nats` tag does.
    NATSImage& with_image(const std::string& reference);

    /// Require authentication: the server starts with `--user <u> --pass <p>`
    /// and `url()` gains `user:pass@`. Set both halves or neither — `start()`
    /// throws at render on half a pair. Test credentials only: the values are
    /// visible in the container's command line via inspect.
    NATSImage& with_username(std::string username);

    /// The password half of `with_username` (same pairing rule).
    NATSImage& with_password(std::string password);

    /// Enable JetStream (`-js`). Stream data lives in the container layer —
    /// gone with the container; `with_reuse` keeps it across runs, or mount a
    /// volume over a `--store_dir` directory through `with_customizer`.
    NATSImage& with_jetstream(bool enable = true);

    /// Append `nats-server` flags after the managed ones, e.g.
    /// `{"--name", "orders-bus"}` or `{"--auth", "tok"}`. Repeatable; calls
    /// accumulate in order. The flags the module renders stay with their
    /// typed setters: an entry naming the credentials (`--user`/`--pass`),
    /// JetStream (`-js`/`--jetstream`), the monitoring listener
    /// (`-m`/`--http_port`), the client listener (`-p`/`--port`,
    /// `-a`/`--addr`/`--net`), or a config file (`-c`/`--config`) makes
    /// `start()` throw up front — the server keeps the LAST occurrence of a
    /// flag, so a duplicate would silently desync the connection getters or
    /// starve the readiness probe (and a config file can re-set any of them
    /// invisibly). The check scans every entry, so a VALUE spelled like a
    /// managed flag (e.g. a server name of "-p") is rejected too. Full
    /// command ownership (config-file setups): a customizer's `with_cmd`
    /// (runs last, wins).
    NATSImage& with_command_args(std::vector<std::string> args);

    /// The single-argument twin of `with_command_args` (same rules).
    NATSImage& with_command_arg(std::string arg);

    // --- Pass-throughs to the underlying builder ---

    /// Set an extra environment variable (for your own tooling — the module
    /// manages no env keys; `nats-server` is configured entirely by its
    /// command line).
    NATSImage& with_env(std::string key, std::string value);

    NATSImage& with_label(std::string key, std::string value);

    /// Join a user-defined network; peers resolve this container by
    /// name/alias at `<alias>:4222` (kClientPort, not the mapped host port).
    NATSImage& with_network(std::string network);
    NATSImage& with_network(const Network& network);
    NATSImage& with_network_alias(std::string alias);

    /// Enable container reuse (effective only when reuse is also enabled
    /// globally — see GenericImage::with_reuse). An adopted server keeps its
    /// JetStream streams: same config, next run, data intact.
    NATSImage& with_reuse(bool reuse = true);

    /// Budget for the whole readiness phase (default: 60s; the server boots
    /// in well under a second — the budget covers slow CI daemons). Image
    /// pull time does not count against it.
    NATSImage& with_startup_timeout(std::chrono::milliseconds timeout);

    /// Retry the whole create→start→wait sequence up to `n` times.
    NATSImage& with_startup_attempts(int n);

    /// Register a callback that customizes the underlying `GenericImage` —
    /// the channel for every option this module does not surface (mounts,
    /// pull policy, ...). Customizers run when the config is rendered
    /// (`start()` / `to_generic()`), in registration order, AFTER the
    /// module's own rendering — what they set wins over the module. A
    /// `with_cmd` here replaces the module's flags wholesale: keep monitoring
    /// on 8222 (or replace the waits too), and keep credentials in the typed
    /// setters — flags set this way desync `url()` and the getters.
    NATSImage& with_customizer(std::function<void(GenericImage&)> customize);

    // --- Getters ---

    /// The configured username; empty when auth is off.
    const std::string& username() const noexcept { return username_; }

    /// The configured password; empty when auth is off.
    const std::string& password() const noexcept { return password_; }

    /// Whether JetStream will be enabled.
    bool jetstream() const noexcept { return jetstream_; }

    /// Extra `nats-server` flags accumulated so far.
    const std::vector<std::string>& command_args() const noexcept { return args_; }

    /// Render the full configuration — module options and customizers applied
    /// — into a plain GenericImage: the drop-down escape hatch when you need
    /// a raw core `Container` (or run-level tweaks) instead of a
    /// NATSContainer. Throws Error — before any daemon contact — on half a
    /// credential pair, or on a module-managed flag inside
    /// `with_command_args` (see there).
    GenericImage to_generic() const;

    /// Create, start, and wait until the server logs readiness and answers
    /// `/healthz`. Throws on failure (`StartupTimeoutError` when the server
    /// never becomes ready within the startup timeout), like
    /// `GenericImage::start()`.
    NATSContainer start() const;

private:
    GenericImage image_;            ///< pin + ports + waits + pass-through state
    std::string username_;          ///< rendered as --user when non-empty
    std::string password_;          ///< rendered as --pass when non-empty
    bool jetstream_ = false;        ///< rendered as -js
    std::vector<std::string> args_; ///< appended after the managed flags
    std::vector<std::function<void(GenericImage&)>> customizers_; ///< applied last
};

/// A running NATS server: connection getters plus the owned container.
///
/// Move-only — it owns the core `Container`, whose destructor force-removes
/// the server (RAII teardown; `container().keep()` or reuse opt out, see
/// Container).
class NATSContainer {
public:
    /// The address a client in the test process connects to. Resolved once,
    /// when the container started.
    const std::string& host() const noexcept { return host_; }

    /// The host port published for the client port 4222. Resolved once, when
    /// the container started; a container restarted by hand gets fresh
    /// ephemeral ports — re-resolve via `container()` then.
    std::uint16_t port() const noexcept { return port_; }

    /// The host port published for the HTTP monitoring port 8222.
    std::uint16_t monitoring_port() const noexcept { return monitoring_port_; }

    /// The username the server requires; empty when auth is off.
    const std::string& username() const noexcept { return username_; }

    /// The password the server requires; empty when auth is off.
    const std::string& password() const noexcept { return password_; }

    /// The server URL, `nats://[user:password@]host:port` — e.g.
    /// `nats://localhost:32771`, or with credentials
    /// `nats://app:s3cr3t@localhost:32771` (percent-encoded). Most clients
    /// accept credentials in the URL; clients that take them separately use
    /// `host()` / `port()` / `username()` / `password()` instead.
    std::string url() const;

    /// The monitoring base URL, e.g. `http://localhost:32772`. The endpoints
    /// (`/healthz`, `/varz`, `/connz`, `/jsz`, ...) answer plain
    /// unauthenticated HTTP GETs — server introspection without a NATS
    /// client library.
    std::string monitoring_url() const;

    /// The underlying container handle: read logs, `stop()` early, `keep()`
    /// it past the test. The default image has no shell or tools — `exec`
    /// has nothing to run (see NATSImage::kDefaultImage).
    Container& container() noexcept { return container_; }
    const Container& container() const noexcept { return container_; }

private:
    friend class NATSImage;
    NATSContainer(Container container, std::string username, std::string password)
        : container_(std::move(container)), username_(std::move(username)),
          password_(std::move(password)), host_(container_.host()),
          port_(container_.get_host_port(tcp(NATSImage::kClientPort))),
          monitoring_port_(container_.get_host_port(tcp(NATSImage::kMonitoringPort))) {}

    Container container_;
    std::string username_;
    std::string password_;
    std::string host_;                  ///< resolved once at start()
    std::uint16_t port_ = 0;            ///< resolved once at start()
    std::uint16_t monitoring_port_ = 0; ///< resolved once at start()
};

} // namespace testcontainers::modules
