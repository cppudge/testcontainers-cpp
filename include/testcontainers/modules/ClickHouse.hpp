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
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"

namespace testcontainers::modules {

class ClickHouseContainer;

/// A reusable, copyable description of a ClickHouse test server: image,
/// credentials, init scripts, and server-config drop-ins. `start()` runs it
/// and returns a `ClickHouseContainer` whose getters hand out the endpoints
/// for both protocols.
///
/// Defaults: `clickhouse:26.3`, user/password/database all "test", ports 8123
/// (HTTP) and 9000 (native protocol) published on ephemeral host ports, and
/// an ORDERED readiness triple: (1) the entrypoint has handed the container
/// over to the real server (the image's first boot runs a TEMPORARY server
/// for provisioning and init scripts that network probes cannot reliably
/// tell apart from the real one — the entrypoint finishing by exec'ing the
/// server over itself can); (2) HTTP `/ping` answering 200 through the
/// published port; (3) an in-container `SELECT 1` over the native protocol,
/// which also proves the provisioned credentials. Every init script has
/// finished by (1). The configured user is the ONLY account: the image
/// removes its built-in `default` user when it provisions a custom one.
///
/// The `with_*` builders mutate in place and return `*this`, so a named config
/// can be configured incrementally and started many times. Core options the
/// module does not surface are reached through `with_customizer`;
/// `to_generic()` drops down to a plain `GenericImage` entirely.
class ClickHouseImage {
public:
    /// The pinned default image. Override with `with_image`; any image that
    /// keeps the official entrypoint contract (CLICKHOUSE_* env,
    /// /docker-entrypoint-initdb.d, clickhouse-client on PATH) works.
    static constexpr std::string_view kDefaultImage = "clickhouse:26.3";

    /// The HTTP-interface port INSIDE the container. Peers on a shared docker
    /// network connect to `<alias-or-name>:kHttpPort`; the test process
    /// itself uses `ClickHouseContainer::http_port()` (the mapped host port).
    static constexpr std::uint16_t kHttpPort = 8123;

    /// The native-protocol port INSIDE the container (clickhouse-cpp,
    /// clickhouse-client). Host side: `ClickHouseContainer::native_port()`.
    static constexpr std::uint16_t kNativePort = 9000;

    /// A config ready to `start()`: image `clickhouse:26.3`, credentials
    /// test/test/test, ports 8123 + 9000 exposed, and the ordered
    /// handover → `/ping` → `SELECT 1` readiness triple.
    ClickHouseImage();

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Override the pinned image with a full reference "name[:tag]" (tag
    /// defaults to "latest").
    ClickHouseImage& with_image(const std::string& reference);

    /// Account provisioned at first boot (CLICKHOUSE_USER). Default "test".
    /// The provisioned user may connect from any address and REPLACES the
    /// image's built-in `default` user.
    ClickHouseImage& with_username(std::string username);

    /// Password for the provisioned account (CLICKHOUSE_PASSWORD). Default
    /// "test". Empty throws at `start()`: the image restricts a passwordless
    /// `default` user to the container's loopback, so the module's host-side
    /// getters could never be true — run a deliberately passwordless server
    /// through `to_generic()` instead. Test credentials only: the value is
    /// visible via inspect.
    ClickHouseImage& with_password(std::string password);

    /// Database created at first boot (CLICKHOUSE_DB). Default "test".
    ClickHouseImage& with_database(std::string database);

    /// Queue a host file for /docker-entrypoint-initdb.d — SQL or shell the
    /// image's provisioning server runs ONCE, at first boot, before the real
    /// server listens (the default readiness probes therefore also wait for
    /// every init script to finish). Repeatable; scripts run in REGISTRATION
    /// order — the module prefixes each container-side name with a
    /// zero-padded index because the entrypoint runs files in name order. The
    /// extension must be one the entrypoint executes — .sql, .sql.gz, or .sh
    /// (anything else throws here rather than being silently ignored in the
    /// container). .sh files are shipped executable. The file is read when
    /// start() copies it.
    ///
    /// Two entrypoint contracts to know: scripts run with NO default
    /// database — qualify names (`CREATE TABLE test.t ...`) or open with
    /// `USE <db>;` (the client runs in multiquery mode) — and a FAILING
    /// script aborts the whole boot, so `start()` fails loudly (the wait
    /// times out; the container's log carries the script's error).
    ///
    /// A reused (adopted) container does NOT re-run init scripts — its data
    /// dir is already initialized; editing a script changes the reuse hash,
    /// so the next start() builds a fresh container with the new schema.
    ClickHouseImage& with_init_script(std::filesystem::path host_path);

    /// In-memory variant: queue `content` under the given file name (a bare
    /// name, no directories; same extension rules and ordering as above).
    ClickHouseImage& with_init_script(const std::string& name, std::string content);

    /// Ship a server-configuration drop-in into /etc/clickhouse-server/config.d
    /// under its own file name. The name must end in .xml, .yaml, or .yml —
    /// the server merges only those (anything else throws here). Do not remap
    /// http_port / tcp_port / listen_host: that breaks the module's port
    /// getters and the readiness probe.
    ClickHouseImage& with_config_file(std::filesystem::path host_config);

    // --- Pass-throughs to the underlying builder ---

    /// Set an extra environment variable — the image's own knobs, e.g.
    /// CLICKHOUSE_DEFAULT_ACCESS_MANAGEMENT=1 to allow SQL CREATE USER/GRANT
    /// from the provisioned account. The credential trio belongs to
    /// with_username/with_password/with_database: the module applies those
    /// env keys last, so they win over raw duplicates set here. Leave
    /// CLICKHOUSE_SKIP_USER_SETUP unset — it disables the provisioning the
    /// getters describe.
    ClickHouseImage& with_env(std::string key, std::string value);

    ClickHouseImage& with_label(std::string key, std::string value);

    /// Join a user-defined network; peers resolve this container by
    /// name/alias at `<alias>:8123` / `<alias>:9000` (the in-container
    /// ports), using the same provisioned credentials.
    ClickHouseImage& with_network(std::string network);
    ClickHouseImage& with_network(const Network& network);
    ClickHouseImage& with_network_alias(std::string alias);

    /// Enable container reuse (effective only when reuse is also enabled
    /// globally — see GenericImage::with_reuse). The natural fit for a seeded
    /// analytics database that should survive across test runs; an adopted
    /// server keeps its data, and init scripts are not re-run.
    ClickHouseImage& with_reuse(bool reuse = true);

    /// REPLACE the default readiness triple with a custom strategy (the
    /// first call drops the module's probes; repeatable — further waits run
    /// in order under the same timeout). Mind the first-boot provisioning
    /// server: network and loopback probes read ready against it (even
    /// through the published port on Docker Desktop) — gate on
    /// `wait_for::successful_shell_command("grep -q clickhouse
    /// /proc/1/comm")` first, as the default triple does.
    ClickHouseImage& with_wait(WaitFor wait);

    /// Budget for the whole readiness phase (default: 60s). Raise it when
    /// init scripts do real work or CI I/O is slow; image pull time does not
    /// count against it.
    ClickHouseImage& with_startup_timeout(std::chrono::milliseconds timeout);

    /// Retry the whole create→start→wait sequence up to `n` times.
    ClickHouseImage& with_startup_attempts(int n);

    /// Register a callback that customizes the underlying `GenericImage` —
    /// the channel for every option this module does not surface (mounts,
    /// ulimits, pull policy, ...). Customizers run when the config is
    /// rendered (`start()` / `to_generic()`), in registration order, AFTER
    /// the module's own rendering — what they set wins over the module. A
    /// wait added here runs IN ADDITION to the default probe (unlike
    /// `with_wait`, which replaces it). Do not set the CLICKHOUSE_*
    /// credential env here: it would desync the credential getters, the DSN,
    /// and `exec_sql` — use the typed setters instead.
    ClickHouseImage& with_customizer(std::function<void(GenericImage&)> customize);

    // --- Getters ---

    const std::string& username() const noexcept { return username_; }
    const std::string& password() const noexcept { return password_; }
    const std::string& database() const noexcept { return database_; }

    /// Render the full configuration — credential env, init-script and
    /// config-drop-in copies, readiness probe, customizers — into a plain
    /// GenericImage: the drop-down escape hatch when you need a raw core
    /// `Container` (or run-level tweaks) instead of a ClickHouseContainer.
    /// Throws Error on an invalid config (empty username, password, or
    /// database) before any daemon contact.
    GenericImage to_generic() const;

    /// Create, start, and wait through the readiness triple: the entrypoint
    /// handed over to the real server (every init script done), `/ping`
    /// answers on the published HTTP port, and an in-container `SELECT 1`
    /// succeeds over the native protocol. Throws Error on config errors
    /// before touching the daemon; DockerError / StartupTimeoutError from
    /// the run itself, like `GenericImage::start()`.
    ClickHouseContainer start() const;

private:
    GenericImage image_;                        ///< pin + ports + pass-through state
    std::string username_{"test"};              ///< CLICKHOUSE_USER, rendered last
    std::string password_{"test"};              ///< CLICKHOUSE_PASSWORD, rendered last
    std::string database_{"test"};              ///< CLICKHOUSE_DB, rendered last
    std::vector<CopyToContainer> init_scripts_; ///< ordered initdb.d copies
    std::vector<CopyToContainer> config_files_; ///< config.d drop-ins
    std::vector<WaitFor> waits_;                ///< empty = the default /ping probe
    std::vector<std::function<void(GenericImage&)>> customizers_; ///< applied last
};

/// A running ClickHouse server: endpoint getters plus the owned container.
///
/// Move-only — it owns the core `Container`, whose destructor force-removes
/// the server (RAII teardown; `container().keep()` or reuse opt out, see
/// Container).
class ClickHouseContainer {
public:
    /// The address a client in the test process connects to. Resolved once,
    /// when the container started.
    const std::string& host() const noexcept { return host_; }

    /// The host port published for the HTTP port 8123. Resolved once, when
    /// the container started; a container restarted by hand gets fresh
    /// ephemeral ports — re-resolve via `container()` then.
    std::uint16_t http_port() const noexcept { return http_port_; }

    /// The host port published for the native-protocol port 9000 — what
    /// clickhouse-cpp's ClientOptions and `clickhouse-client --port` take.
    std::uint16_t native_port() const noexcept { return native_port_; }

    const std::string& username() const noexcept { return username_; }
    const std::string& password() const noexcept { return password_; }
    const std::string& database() const noexcept { return database_; }

    /// Native-protocol DSN,
    /// `clickhouse://user:password@host:native_port/database`
    /// (percent-encoded) — e.g. `clickhouse://test:test@localhost:32768/test`.
    /// clickhouse-cpp takes the discrete pieces instead: `host()`,
    /// `native_port()`, `username()`, `password()`, `database()`. HTTP
    /// consumers use `http_url()` — swapping this DSN's scheme would keep the
    /// wrong port.
    std::string connection_string() const;

    /// The HTTP base URL, e.g. `http://localhost:32769`. Credentials go per
    /// request (basic auth, or the X-ClickHouse-User / X-ClickHouse-Key
    /// headers); JDBC-style URLs build on the same endpoint.
    std::string http_url() const;

    /// Run one SQL statement through the in-container `clickhouse-client`
    /// (native protocol over the container's loopback, the provisioned user,
    /// `database()`) — a zero-dependency seed/assert helper. Output is
    /// TabSeparated: one row per line, columns tab-separated, no headers. A
    /// failing statement is reported via the result's exit_code/stderr_data,
    /// not thrown.
    ExecResult exec_sql(const std::string& sql) const;

    /// The underlying container handle: exec `clickhouse-client`, read logs,
    /// copy files, `stop()` early, `keep()` it past the test.
    Container& container() noexcept { return container_; }
    const Container& container() const noexcept { return container_; }

private:
    friend class ClickHouseImage;
    ClickHouseContainer(Container container, std::string username, std::string password,
                        std::string database)
        : container_(std::move(container)), username_(std::move(username)),
          password_(std::move(password)), database_(std::move(database)), host_(container_.host()),
          http_port_(container_.get_host_port(tcp(ClickHouseImage::kHttpPort))),
          native_port_(container_.get_host_port(tcp(ClickHouseImage::kNativePort))) {}

    Container container_;
    std::string username_;
    std::string password_;
    std::string database_;
    std::string host_;              ///< resolved once at start()
    std::uint16_t http_port_ = 0;   ///< resolved once at start()
    std::uint16_t native_port_ = 0; ///< resolved once at start()
};

} // namespace testcontainers::modules
