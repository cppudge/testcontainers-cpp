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

class MariaDBContainer;

/// A reusable, copyable description of a MariaDB server container. `start()`
/// returns a `MariaDBContainer` handle owning the running container and
/// exposing the connection getters.
///
/// Defaults mirror the MySQL module: user "test" / password "test" /
/// database "test" (root shares the password), port 3306 published on an
/// ephemeral host port. Readiness uses the probe the image itself ships —
/// `healthcheck.sh --connect --innodb_initialized` — which needs no
/// credentials and only reports ready once the REAL server (not the
/// bootstrap instance that runs init scripts) accepts TCP connections, so
/// init-script data is visible when `start()` returns.
///
/// Configuration is provisioned through the MARIADB_* environment contract;
/// builder semantics are identical to `MySQLImage`'s.
class MariaDBImage {
public:
    /// The pinned default image (the rolling LTS line). Override with
    /// `with_image`; any image honoring the official env contract
    /// (MARIADB_ROOT_PASSWORD / MARIADB_USER / MARIADB_PASSWORD /
    /// MARIADB_DATABASE, /docker-entrypoint-initdb.d) works.
    static constexpr std::string_view kDefaultImage = "mariadb:11";

    /// The server port INSIDE the container. Peers on a shared docker
    /// network connect to `<alias-or-name>:kPort`; the test process itself
    /// uses `MariaDBContainer::port()` (the mapped host port) instead.
    static constexpr std::uint16_t kPort = 3306;

    /// A config ready to `start()`: image `mariadb:11`, credentials
    /// test/test/test, port 3306 exposed, the image's own `healthcheck.sh`
    /// readiness probe, and a 120s startup budget (a first boot initializes
    /// the data directory and routinely takes tens of seconds on CI).
    MariaDBImage();

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Override the pinned image with a full reference "name[:tag]" (tag
    /// defaults to "latest").
    MariaDBImage& with_image(const std::string& reference);

    /// The user provisioned on first boot (default "test") with full
    /// privileges on `database()`. "root" (any letter case — stored
    /// lowercase, since the real account is 'root') provisions no separate
    /// user; the credential getters then describe root itself.
    MariaDBImage& with_username(std::string username);

    /// The password (default "test"); the same value becomes the root
    /// password. Empty is only valid with `with_username("root")`
    /// (passwordless root via MARIADB_ALLOW_EMPTY_ROOT_PASSWORD); any other
    /// user with an empty password makes `start()` throw up front. Test
    /// credentials only: the value is visible via inspect.
    MariaDBImage& with_password(std::string password) {
        opts_.password = std::move(password);
        return *this;
    }

    /// The database created on first boot (default "test"). An empty name
    /// creates none and drops the path from `connection_string()`.
    MariaDBImage& with_database(std::string database) {
        opts_.database = std::move(database);
        return *this;
    }

    /// Queue a host file for /docker-entrypoint-initdb.d — runs once, on
    /// first boot, in REGISTRATION order (a zero-padded index prefix pins
    /// it), before `start()` returns. Recognized extensions: .sql, .sql.gz,
    /// .sql.xz, .sql.zst, .sh (anything else throws here, since the image
    /// would silently skip it); .sh ships executable. A reused (adopted)
    /// container does not re-run init scripts; editing one changes the
    /// reuse hash, so a fresh container is created.
    MariaDBImage& with_init_script(std::filesystem::path host_path);

    /// In-memory variant: queue `content` under the given file name (a bare
    /// name, no directories; same extension rules and ordering as above).
    MariaDBImage& with_init_script(const std::string& name, std::string content);

    /// Append one server option to the container command, e.g.
    /// `with_command_arg("--character-set-server=utf8mb4")`. Option
    /// arguments (starting with '-') are forwarded to mariadbd by the
    /// entrypoint. Do not change `--port` (the probe and `port()` assume
    /// 3306 inside the container).
    MariaDBImage& with_command_arg(std::string arg) {
        opts_.command_args.push_back(std::move(arg));
        return *this;
    }

    /// Append several server options at once — the batch twin of
    /// `with_command_arg`. Repeatable; calls accumulate in order.
    MariaDBImage& with_command_args(std::vector<std::string> args) {
        for (std::string& arg : args) {
            opts_.command_args.push_back(std::move(arg));
        }
        return *this;
    }

    /// Copy a .cnf file into /etc/mysql/conf.d (included by the image's
    /// configuration; the name must end in ".cnf" or this throws). Copied
    /// with mode 0644 — the server ignores world-writable config files.
    MariaDBImage& with_config_file(std::filesystem::path host_cnf);

    // --- Pass-throughs to the underlying builder ---

    /// Set an extra environment variable (e.g. MARIADB_AUTO_UPGRADE). The
    /// managed MARIADB_* credential keys belong to the typed setters: the
    /// module applies them last, so they win over raw duplicates set here.
    MariaDBImage& with_env(std::string key, std::string value) {
        image_.with_env(std::move(key), std::move(value));
        return *this;
    }

    MariaDBImage& with_label(std::string key, std::string value) {
        image_.with_label(std::move(key), std::move(value));
        return *this;
    }

    /// Join a user-defined network; peers resolve this container by
    /// name/alias at `<alias>:3306` (kPort, not the mapped host port).
    MariaDBImage& with_network(std::string network) {
        image_.with_network(std::move(network));
        return *this;
    }
    MariaDBImage& with_network(const Network& network);
    MariaDBImage& with_network_alias(std::string alias) {
        image_.with_network_alias(std::move(alias));
        return *this;
    }

    /// Enable container reuse (effective only when reuse is also enabled
    /// globally — see GenericImage::with_reuse). Init scripts do not re-run
    /// on an adopted container: its data directory is already initialized.
    MariaDBImage& with_reuse(bool reuse = true) {
        image_.with_reuse(reuse);
        return *this;
    }

    /// REPLACE the default readiness probe with a custom strategy (the first
    /// call drops the module's probe; repeatable — further waits run in
    /// order under the same timeout). The default probe forces TCP for a
    /// reason: the image's first boot runs a temporary socket-only server
    /// while init scripts execute.
    MariaDBImage& with_wait(WaitFor wait) {
        opts_.waits.push_back(std::move(wait));
        return *this;
    }

    /// Replace the default 120s startup budget. Image pull time does not
    /// count against it.
    MariaDBImage& with_startup_timeout(std::chrono::milliseconds timeout) {
        image_.with_startup_timeout(timeout);
        return *this;
    }

    /// Retry the whole create→start→wait sequence up to `n` times.
    MariaDBImage& with_startup_attempts(int n) {
        image_.with_startup_attempts(n);
        return *this;
    }

    /// Register a callback that customizes the underlying `GenericImage` —
    /// the channel for every option this module does not surface.
    /// Customizers run when the config is rendered (`start()` /
    /// `to_generic()`), in registration order, AFTER the module's own
    /// rendering — what they set wins over the module. Do not set the
    /// managed MARIADB_* credential env here: it would desync the credential
    /// getters and the DSN.
    MariaDBImage& with_customizer(std::function<void(GenericImage&)> customize) {
        opts_.customizers.push_back(std::move(customize));
        return *this;
    }

    // --- Getters ---

    const std::string& username() const noexcept { return opts_.username; }
    const std::string& password() const noexcept { return opts_.password; }
    const std::string& database() const noexcept { return opts_.database; }

    /// Render the full configuration into a plain GenericImage (the
    /// drop-down escape hatch); same contract and validation as
    /// `MySQLImage::to_generic`.
    GenericImage to_generic() const;

    /// Create, start, and wait until the image's healthcheck reports the
    /// real server ready (init scripts complete). Throws Error on config
    /// errors, DockerError on daemon failures, StartupTimeoutError when
    /// readiness never arrives — also the symptom of a failing init script
    /// (check the container logs).
    MariaDBContainer start() const;

private:
    GenericImage image_;              ///< pin + port + timeout + pass-through state
    detail::MySqlFamilyOptions opts_; ///< the domain knobs, rendered at start()
};

/// A running MariaDB server: connection getters plus the owned container.
///
/// Move-only — it owns the core `Container`, whose destructor force-removes the
/// server (RAII teardown; `container().keep()` or reuse opt out, see
/// Container).
class MariaDBContainer {
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

    /// Root's password — always equal to `password()` by construction
    /// (empty only for a passwordless-root config).
    const std::string& root_password() const noexcept { return password_; }

    const std::string& database() const noexcept { return database_; }

    /// The server URL, `mysql://user[:password]@host:port[/database]` —
    /// MariaDB speaks the MySQL wire protocol, and the mysql scheme is what
    /// URL-parsing clients accept ("mariadb://" is widely rejected). No path
    /// segment when the database name is empty; components percent-encoded.
    std::string connection_string() const;

    /// The underlying container. The in-container client binaries carry the
    /// mariadb-prefixed names (`mariadb`, `mariadb-admin`, `mariadb-dump`) —
    /// the historical mysql-prefixed names are deprecated in this image.
    /// E.g. `container().exec({"mariadb", "-h127.0.0.1", "-utest", "-ptest",
    /// "-Dtest", "-e", "SELECT 1"})`.
    Container& container() noexcept { return container_; }
    const Container& container() const noexcept { return container_; }

private:
    friend class MariaDBImage;
    MariaDBContainer(Container container, std::string username, std::string password,
                     std::string database)
        : container_(std::move(container)), username_(std::move(username)),
          password_(std::move(password)), database_(std::move(database)), host_(container_.host()),
          port_(container_.get_host_port(tcp(MariaDBImage::kPort))) {}

    Container container_;
    std::string username_;
    std::string password_;
    std::string database_;
    std::string host_;       ///< resolved once at start()
    std::uint16_t port_ = 0; ///< resolved once at start()
};

} // namespace testcontainers::modules
