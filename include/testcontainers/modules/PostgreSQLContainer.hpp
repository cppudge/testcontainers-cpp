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

class StartedPostgreSQL;

/// A reusable, copyable description of a PostgreSQL test database: image,
/// credentials, init scripts, and server configuration. `start()` runs it and
/// returns a `StartedPostgreSQL` whose getters hand out ready-to-paste
/// connection strings.
///
/// Defaults: `postgres:16-alpine`, user/password/database all "test", port
/// 5432 published on an ephemeral host port, readiness = an in-container
/// `pg_isready -h 127.0.0.1` probe (immune to the image's initialization
/// restart — the temporary init server never listens on TCP, so TCP readiness
/// also proves every init script finished).
///
/// The `with_*` builders mutate in place and return `*this`, so a named config
/// can be configured incrementally and started many times. Core options the
/// module does not surface are reached through `with_customizer`;
/// `to_generic()` drops down to a plain `GenericImage` entirely.
class PostgreSQLContainer {
public:
    /// The pinned default image. Override with `with_image`; any
    /// postgres-derived image that keeps the official entrypoint contract
    /// (POSTGRES_* env, /docker-entrypoint-initdb.d, pg_isready on PATH)
    /// works — e.g. "pgvector/pgvector:pg16" or "timescale/timescaledb".
    /// Note the alpine pin runs with a C locale; for collation-sensitive
    /// tests use "postgres:16" (Debian) or POSTGRES_INITDB_ARGS.
    static constexpr std::string_view kDefaultImage = "postgres:16-alpine";

    /// The server port INSIDE the container. Peers on a shared docker network
    /// connect to `<alias-or-name>:kPort`; the test process itself uses
    /// `StartedPostgreSQL::port()` (the mapped host port) instead.
    static constexpr std::uint16_t kPort = 5432;

    /// A config ready to `start()`: image `postgres:16-alpine`, credentials
    /// test/test/test, port 5432 exposed, `pg_isready` readiness probe.
    PostgreSQLContainer();

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Override the pinned image with a full reference "name[:tag]" (tag
    /// defaults to "latest").
    PostgreSQLContainer& with_image(const std::string& reference);

    /// Superuser name the database is initialized with (POSTGRES_USER).
    /// Default "test".
    PostgreSQLContainer& with_username(std::string username);

    /// Superuser password (POSTGRES_PASSWORD). Default "test". The postgres
    /// image refuses to boot with an EMPTY password, so `start()` throws
    /// immediately in that case — unless `with_env("POSTGRES_HOST_AUTH_METHOD",
    /// "trust")` was also set to run a deliberately passwordless server.
    /// Test credentials only: the value is visible via inspect.
    PostgreSQLContainer& with_password(std::string password);

    /// Name of the database created at first boot (POSTGRES_DB). Default "test".
    PostgreSQLContainer& with_database(std::string database);

    /// Queue a host file for /docker-entrypoint-initdb.d — SQL or shell the
    /// server runs ONCE, at first boot, before it accepts TCP connections
    /// (the default readiness probe therefore also waits for every init
    /// script to finish). Repeatable; scripts run in REGISTRATION order — the
    /// module prefixes each container-side name with a zero-padded index
    /// ("0000-schema.sql", "0001-seed.sql") because the entrypoint runs files
    /// in name order. The extension picks the execution mode and must be one
    /// the entrypoint knows — .sql, .sql.gz, .sql.xz, .sql.zst, or .sh
    /// (anything else throws here rather than being silently ignored in the
    /// container). .sh files are shipped executable so they run as standalone
    /// scripts. The file is read when start() copies it.
    ///
    /// A reused (adopted) container does NOT re-run init scripts — its data
    /// dir is already initialized, which is the point of reuse; editing a
    /// script changes the reuse hash, so the next start() builds a fresh
    /// container with the new schema.
    PostgreSQLContainer& with_init_script(std::filesystem::path host_path);

    /// In-memory variant: queue `content` under the given file name (a bare
    /// name, no directories; same extension rules and ordering as above).
    PostgreSQLContainer& with_init_script(const std::string& name, std::string content);

    /// Append a server configuration parameter, passed as `postgres -c
    /// key=value` on the container command line — e.g.
    /// `with_config_option("fsync", "off")` (the classic test-suite speedup)
    /// or "wal_level"/"logical" for CDC tests. Repeatable; for a key given
    /// twice the server applies the LAST value. Setting "port" or
    /// "listen_addresses" breaks the module's port getter and readiness
    /// probe — leave those alone.
    PostgreSQLContainer& with_config_option(std::string key, std::string value);

    // --- Pass-throughs to the underlying builder ---

    /// Set an extra environment variable — the image's own knobs
    /// (POSTGRES_INITDB_ARGS, POSTGRES_HOST_AUTH_METHOD, PGDATA, ...). The
    /// credential trio belongs to with_username/with_password/with_database:
    /// the module applies those env keys last, so they win over raw
    /// duplicates set here.
    PostgreSQLContainer& with_env(std::string key, std::string value);

    PostgreSQLContainer& with_label(std::string key, std::string value);

    /// Join a user-defined network; peers resolve this container by
    /// name/alias at `<alias>:5432` (kPort, not the mapped host port).
    PostgreSQLContainer& with_network(std::string network);
    PostgreSQLContainer& with_network(const Network& network);
    PostgreSQLContainer& with_network_alias(std::string alias);

    /// Enable container reuse (effective only when reuse is also enabled
    /// globally — see GenericImage::with_reuse for the full semantics). The
    /// natural fit for a seeded database that should survive across test
    /// runs: same config, and the next run adopts the running server with
    /// its data intact.
    PostgreSQLContainer& with_reuse(bool reuse = true);

    /// REPLACE the default readiness probe with a custom strategy (the first
    /// call drops the module's pg_isready probe; repeatable — further waits
    /// run in order under the same timeout). Note a plain `pg_isready`
    /// WITHOUT "-h 127.0.0.1" checks the unix socket and reads ready during
    /// the image's initialization window — keep the "-h".
    PostgreSQLContainer& with_wait(WaitFor wait);

    /// Budget for the whole readiness phase (default: 60s). Raise it when
    /// init scripts do real work or CI I/O is slow; image pull time does not
    /// count against it.
    PostgreSQLContainer& with_startup_timeout(std::chrono::milliseconds timeout);

    /// Retry the whole create→start→wait sequence up to `n` times.
    PostgreSQLContainer& with_startup_attempts(int n);

    /// Register a callback that customizes the underlying `GenericImage` —
    /// the channel for every option this module does not surface (mounts,
    /// shm size, pull policy, ...). Customizers run when the config is
    /// rendered (`start()` / `to_generic()`), in registration order, AFTER
    /// the module's own rendering — what they set wins over the module. A
    /// wait added here runs IN ADDITION to the default probe (unlike
    /// `with_wait`, which replaces it). Do not set the POSTGRES_* credential
    /// env here: it would desync the credential getters, the DSNs, and
    /// `exec_sql`, and the empty-password check does not see it — use
    /// with_username/with_password/with_database (and `with_env` for trust
    /// auth) instead.
    PostgreSQLContainer& with_customizer(std::function<void(GenericImage&)> customize);

    // --- Getters ---

    const std::string& username() const noexcept { return username_; }
    const std::string& password() const noexcept { return password_; }
    const std::string& database() const noexcept { return database_; }

    /// Render the full configuration — credentials env, init-script copies,
    /// config-option command line, readiness probe, customizers — into a
    /// plain GenericImage: the drop-down escape hatch when you need a raw
    /// `Container` (or run-level tweaks) instead of a StartedPostgreSQL.
    /// Throws Error on an invalid config (empty username/database; empty
    /// password without trust auth) before any daemon contact.
    GenericImage to_generic() const;

    /// Create, start, and wait until the server accepts TCP connections (by
    /// which point every init script has run). Throws Error on config errors
    /// before touching the daemon; DockerError / StartupTimeoutError from the
    /// run itself, like `GenericImage::start()`.
    StartedPostgreSQL start() const;

private:
    GenericImage image_;                        ///< pin + port + pass-through state
    std::string username_{"test"};              ///< POSTGRES_USER, rendered last
    std::string password_{"test"};              ///< POSTGRES_PASSWORD, rendered last
    std::string database_{"test"};              ///< POSTGRES_DB, rendered last
    std::vector<CopyToContainer> init_scripts_; ///< ordered initdb.d copies
    std::vector<std::pair<std::string, std::string>> config_options_; ///< -> `postgres -c`
    std::vector<WaitFor> waits_; ///< empty = the default pg_isready probe
    std::vector<std::function<void(GenericImage&)>> customizers_; ///< applied last
};

/// A running PostgreSQL server: connection getters plus the owned container.
///
/// Move-only — it owns the `Container`, whose destructor force-removes the
/// server (RAII teardown; `container().keep()` or reuse opt out, see
/// Container).
class StartedPostgreSQL {
public:
    /// The address a client in the test process connects to. Resolved once,
    /// when the container started.
    const std::string& host() const noexcept { return host_; }

    /// The host port published for the server port 5432. Resolved once, when
    /// the container started; a container restarted by hand gets fresh
    /// ephemeral ports — re-resolve via `container()` then.
    std::uint16_t port() const noexcept { return port_; }

    const std::string& username() const noexcept { return username_; }
    const std::string& password() const noexcept { return password_; }
    const std::string& database() const noexcept { return database_; }

    /// URI-form DSN, `scheme://user[:password]@host:port/database`, every
    /// component percent-encoded — paste into PQconnectdb / pqxx::connection.
    /// The scheme parameter serves polyglot consumers ("postgres" for
    /// Go-style tools, "postgresql+psycopg" for SQLAlchemy, ...). For extra
    /// query parameters (sslmode=disable and friends), rebuild with
    /// ConnectionString and the discrete getters.
    ///
    /// This DSN is host-side: a SIBLING CONTAINER on a shared network dials
    /// `<alias>:5432` (PostgreSQLContainer::kPort) instead — build that DSN
    /// with ConnectionString, your alias, and kPort.
    std::string connection_string(const std::string& scheme = "postgresql") const;

    /// libpq keyword/value form of the same DSN: `host=... port=...
    /// dbname=... user=... password=...`, values quoted per libpq rules (the
    /// password keyword is omitted when empty). Equivalent input to
    /// PQconnectdb and friends — take whichever form your code base prefers.
    std::string conninfo() const;

    /// Run one SQL command through the in-container psql
    /// (`psql -v ON_ERROR_STOP=1 -X -tA -U <user> -d <db> -c <sql>`) — a
    /// zero-dependency seed/assert helper. Output is unaligned tuples-only:
    /// one row per line, columns '|'-separated, no headers. Runs over the
    /// container-local socket (no password needed). A failing statement is
    /// reported via the result's exit_code/stderr_data, not thrown.
    ExecResult exec_sql(const std::string& sql) const;

    /// The underlying container handle: exec, read logs, copy files,
    /// `stop()` early, `keep()` it past the test.
    Container& container() noexcept { return container_; }
    const Container& container() const noexcept { return container_; }

private:
    friend class PostgreSQLContainer;
    StartedPostgreSQL(Container container, std::string username, std::string password,
                      std::string database)
        : container_(std::move(container)), username_(std::move(username)),
          password_(std::move(password)), database_(std::move(database)), host_(container_.host()),
          port_(container_.get_host_port(tcp(PostgreSQLContainer::kPort))) {}

    Container container_;
    std::string username_;
    std::string password_;
    std::string database_;
    std::string host_;       ///< resolved once at start()
    std::uint16_t port_ = 0; ///< resolved once at start()
};

} // namespace testcontainers::modules
