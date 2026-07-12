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
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/modules/detail/MySqlFamily.hpp"

namespace testcontainers::modules {

class StartedMySQL;

/// A reusable, copyable description of a MySQL server container. `start()`
/// returns a `StartedMySQL` handle that owns the running container and
/// exposes the connection getters (host, port, credentials, URL).
///
/// Out of the box the server is provisioned with user "test" / password
/// "test" / database "test" (root shares the same password), publishes port
/// 3306 on an ephemeral host port, and `start()` returns only once the
/// server accepts TCP connections — by which point any registered init
/// scripts have already run, so the data they create is visible from the
/// first query.
///
/// The `with_*` builders mutate in place and return `*this` by reference, so
/// a named config can be configured incrementally and started many times.
/// Core options the module does not surface are reached through
/// `with_customizer`; `to_generic()` drops down to a plain `GenericImage`.
class MySQLContainer {
public:
    /// The pinned default image (the LTS line). Override with `with_image`;
    /// any image honoring the official env contract (MYSQL_ROOT_PASSWORD /
    /// MYSQL_USER / MYSQL_PASSWORD / MYSQL_DATABASE,
    /// /docker-entrypoint-initdb.d) works too, e.g. percona variants.
    static constexpr std::string_view kDefaultImage = "mysql:8.4";

    /// The server port INSIDE the container. Peers on a shared docker
    /// network connect to `<alias-or-name>:kPort`; the test process itself
    /// uses `StartedMySQL::port()` (the mapped host port) instead.
    static constexpr std::uint16_t kPort = 3306;

    /// A config ready to `start()`: image `mysql:8.4`, credentials
    /// test/test/test, port 3306 exposed, a TCP-forced `mysqladmin ping`
    /// readiness probe, and a 120s startup budget (a first boot initializes
    /// the data directory and routinely takes tens of seconds on CI).
    MySQLContainer();

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Override the pinned image with a full reference "name[:tag]" (tag
    /// defaults to "latest").
    MySQLContainer& with_image(const std::string& reference);

    /// The user the server is provisioned with (default "test"), created on
    /// first boot with full privileges on `database()`. Special case: "root"
    /// (any letter case — stored lowercase, since the real account is
    /// 'root') provisions no separate user; the credential getters then
    /// describe the root account itself.
    MySQLContainer& with_username(std::string username);

    /// The password (default "test"). The SAME value also becomes the root
    /// password, so tests always hold a known superuser. An empty password
    /// is only valid together with `with_username("root")` (passwordless
    /// root); any other user with an empty password makes `start()` throw up
    /// front instead of booting an image that would refuse to start. Test
    /// credentials only: the value is visible via inspect.
    MySQLContainer& with_password(std::string password) {
        opts_.password = std::move(password);
        return *this;
    }

    /// The database created on first boot (default "test"); `username()` is
    /// granted all privileges on it. An empty name creates no database (and
    /// drops the path segment from `connection_string()`) — connect as root
    /// and manage schemas yourself.
    MySQLContainer& with_database(std::string database) {
        opts_.database = std::move(database);
        return *this;
    }

    /// Queue a host file for /docker-entrypoint-initdb.d. Init scripts run
    /// ONCE, on the container's first boot, in REGISTRATION order (the
    /// module prefixes each container-side name with a zero-padded index —
    /// the image runs files in name order), and they complete before
    /// `start()` returns. The extension picks the execution mode and must be
    /// one the image knows — .sql, .sql.gz, .sql.xz, .sql.zst, or .sh
    /// (anything else throws here rather than being silently skipped). .sh
    /// ships executable so it runs as a standalone script. A reused
    /// (adopted) container does not re-run init scripts; editing one changes
    /// the reuse hash, so a fresh container is created.
    MySQLContainer& with_init_script(std::filesystem::path host_path);

    /// In-memory variant: queue `content` under the given file name (a bare
    /// name, no directories; same extension rules and ordering as above).
    MySQLContainer& with_init_script(const std::string& name, std::string content);

    /// Append one server option to the container command, e.g.
    /// `with_command_arg("--character-set-server=utf8mb4")`. The image's
    /// entrypoint forwards option arguments (starting with '-') to mysqld.
    /// Do not change `--port` here: the readiness probe and `port()` assume
    /// the in-container port stays 3306. Note for pre-8.0-era client stacks:
    /// MySQL 8.4 disables the mysql_native_password plugin by default —
    /// re-enable it with `with_command_arg("--mysql-native-password=ON")`
    /// plus an init script altering your user to that plugin.
    MySQLContainer& with_command_arg(std::string arg) {
        opts_.command_args.push_back(std::move(arg));
        return *this;
    }

    /// Append several server options at once — the batch twin of
    /// `with_command_arg`. Repeatable; calls accumulate in order.
    MySQLContainer& with_command_args(std::vector<std::string> args) {
        for (std::string& arg : args) {
            opts_.command_args.push_back(std::move(arg));
        }
        return *this;
    }

    /// Copy a configuration file into /etc/mysql/conf.d, which the image's
    /// my.cnf includes. The file name must end in ".cnf" (the include glob;
    /// anything else throws here). Copied with mode 0644 — the server
    /// ignores world-writable config files. Add several to layer several.
    MySQLContainer& with_config_file(std::filesystem::path host_cnf);

    // --- Pass-throughs to the underlying builder ---

    /// Set an extra environment variable — image switches beyond the typed
    /// setters. The managed MYSQL_* credential keys belong to
    /// with_username/with_password/with_database: the module applies them
    /// last, so they win over raw duplicates set here.
    MySQLContainer& with_env(std::string key, std::string value) {
        image_.with_env(std::move(key), std::move(value));
        return *this;
    }

    MySQLContainer& with_label(std::string key, std::string value) {
        image_.with_label(std::move(key), std::move(value));
        return *this;
    }

    /// Join a user-defined network; peers resolve this container by
    /// name/alias at `<alias>:3306` (kPort, not the mapped host port).
    MySQLContainer& with_network(std::string network) {
        image_.with_network(std::move(network));
        return *this;
    }
    MySQLContainer& with_network(const Network& network);
    MySQLContainer& with_network_alias(std::string alias) {
        image_.with_network_alias(std::move(alias));
        return *this;
    }

    /// Enable container reuse (effective only when reuse is also enabled
    /// globally — see GenericImage::with_reuse). Init scripts do not re-run
    /// on an adopted container: its data directory is already initialized.
    MySQLContainer& with_reuse(bool reuse = true) {
        image_.with_reuse(reuse);
        return *this;
    }

    /// REPLACE the default readiness probe with a custom strategy (the first
    /// call drops the module's probe; repeatable — further waits run in
    /// order under the same timeout). Note the default probe forces TCP for
    /// a reason: the image's first boot runs a temporary socket-only server
    /// while init scripts execute.
    MySQLContainer& with_wait(WaitFor wait) {
        opts_.waits.push_back(std::move(wait));
        return *this;
    }

    /// Replace the default 120s startup budget. Image pull time does not
    /// count against it.
    MySQLContainer& with_startup_timeout(std::chrono::milliseconds timeout) {
        image_.with_startup_timeout(timeout);
        return *this;
    }

    /// Retry the whole create→start→wait sequence up to `n` times.
    MySQLContainer& with_startup_attempts(int n) {
        image_.with_startup_attempts(n);
        return *this;
    }

    /// Register a callback that customizes the underlying `GenericImage` —
    /// the channel for every option this module does not surface (mounts,
    /// memory limits, pull policy, ...). Customizers run when the config is
    /// rendered (`start()` / `to_generic()`), in registration order, AFTER
    /// the module's own rendering — what they set wins over the module. Do
    /// not set the managed MYSQL_* credential env here: it would desync the
    /// credential getters, the DSN, and the readiness probe.
    MySQLContainer& with_customizer(std::function<void(GenericImage&)> customize) {
        opts_.customizers.push_back(std::move(customize));
        return *this;
    }

    // --- Getters ---

    const std::string& username() const noexcept { return opts_.username; }
    const std::string& password() const noexcept { return opts_.password; }
    const std::string& database() const noexcept { return opts_.database; }

    /// Render the full configuration — credential env matrix, staged
    /// copies, command args, readiness probe, customizers — into a plain
    /// GenericImage: the drop-down escape hatch when you need a raw
    /// `Container` (or run-level tweaks) instead of a StartedMySQL. Throws
    /// Error on an invalid config (empty username; empty password with a
    /// non-root user) before any daemon contact.
    GenericImage to_generic() const;

    /// Create, start, and wait until the server accepts TCP connections (by
    /// which point every init script has run). Throws Error on config
    /// errors, DockerError on daemon failures, and StartupTimeoutError when
    /// readiness never arrives — which is also how a FAILING init script
    /// surfaces (the server exits and the budget runs out); check the
    /// container logs when diagnosing one.
    StartedMySQL start() const;

private:
    GenericImage image_;              ///< pin + port + timeout + pass-through state
    detail::MySqlFamilyOptions opts_; ///< the domain knobs, rendered at start()
};

/// A running MySQL server: connection getters plus the owned container.
///
/// Move-only — it owns the `Container`, whose destructor force-removes the
/// server (RAII teardown; `container().keep()` or reuse opt out, see
/// Container).
class StartedMySQL {
public:
    /// The address a client in the test process connects to. Resolved once,
    /// when the container started.
    const std::string& host() const noexcept { return host_; }

    /// The host port published for the server port 3306. Resolved once, when
    /// the container started; a container restarted by hand gets fresh
    /// ephemeral ports — re-resolve via `container()` then.
    std::uint16_t port() const noexcept { return port_; }

    const std::string& username() const noexcept { return username_; }
    const std::string& password() const noexcept { return password_; }

    /// Root's password — always the same value as `password()` by
    /// construction (empty only for a passwordless-root config). Exists so
    /// call sites that connect as root read correctly.
    const std::string& root_password() const noexcept { return password_; }

    const std::string& database() const noexcept { return database_; }

    /// The server URL, `mysql://user[:password]@host:port[/database]` (no
    /// path segment when the database name is empty). User, password, and
    /// database are percent-encoded, so credentials with '@' or ':' survive.
    /// Clients taking discrete parameters (mysql_real_connect and friends)
    /// should prefer the component getters.
    std::string connection_string() const;

    /// The underlying container — exec, logs, copy files, `stop()` early,
    /// `keep()` it past the test. The in-container client binary is `mysql`,
    /// e.g. `container().exec({"mysql", "-h127.0.0.1", "-utest", "-ptest",
    /// "-Dtest", "-e", "SELECT 1"})`.
    Container& container() noexcept { return container_; }
    const Container& container() const noexcept { return container_; }

private:
    friend class MySQLContainer;
    StartedMySQL(Container container, std::string username, std::string password,
                 std::string database)
        : container_(std::move(container)), username_(std::move(username)),
          password_(std::move(password)), database_(std::move(database)), host_(container_.host()),
          port_(container_.get_host_port(tcp(MySQLContainer::kPort))) {}

    Container container_;
    std::string username_;
    std::string password_;
    std::string database_;
    std::string host_;       ///< resolved once at start()
    std::uint16_t port_ = 0; ///< resolved once at start()
};

} // namespace testcontainers::modules
