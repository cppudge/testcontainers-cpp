#include "MySqlFamily.hpp"

#include <cctype>
#include <chrono>
#include <string_view>
#include <utility>

#include "testcontainers/ConnectionString.hpp"
#include "testcontainers/Error.hpp"

namespace testcontainers::modules::detail {

namespace {

/// The extensions both images' entrypoints execute; anything else they SKIP
/// silently — a green start with an empty schema — so registration refuses
/// unknown ones up front.
constexpr std::string_view kInitExtensions[] = {".sql", ".sql.gz", ".sql.xz", ".sql.zst", ".sh"};

bool has_known_init_extension(std::string_view name) {
    for (const std::string_view ext : kInitExtensions) {
        if (name.size() > ext.size() && name.ends_with(ext)) {
            return true;
        }
    }
    return false;
}

/// "0000-name" — both entrypoints run initdb.d files in C-collation name
/// order, so a zero-padded registration index makes registration order the
/// execution order (the guards below throw before the index outgrows it).
std::string indexed_name(std::size_t index, const std::string& name) {
    std::string digits = std::to_string(index);
    if (digits.size() < 4) {
        digits.insert(0, 4 - digits.size(), '0');
    }
    return digits + "-" + name;
}

void stage_init_copy(MySqlFamilyOptions& opts, const std::string& name, CopyToContainer copy) {
    if (name.ends_with(".sh")) {
        // Executable, so the entrypoint runs it as a standalone script; a
        // non-executable .sh is SOURCED into the entrypoint's shell instead
        // (where a stray `exit` kills the boot).
        copy.with_mode(0755);
    }
    opts.init_scripts.push_back(std::move(copy));
}

void check_init_name(const MySqlFamilyOptions& opts, const std::string& name) {
    if (!has_known_init_extension(name)) {
        throw Error("init script \"" + name +
                    "\" has an extension the image's entrypoint would silently ignore; "
                    "use .sql, .sql.gz, .sql.xz, .sql.zst, or .sh");
    }
    if (opts.init_scripts.size() >= 10000) {
        throw Error("too many init scripts (the 0000- ordering prefix is four digits)");
    }
}

std::vector<std::string> mysql_ready_probe(const MySqlFamilyOptions& opts) {
    // -h127.0.0.1 forces TCP, which the temporary bootstrap instance
    // (--skip-networking) never serves — see render() below. mysqladmin ping
    // exits 0 even on access-denied (it measures liveness), so the probe is
    // immune to credential edge cases; passing the configured credentials
    // anyway exercises the normal auth path.
    std::vector<std::string> argv{"mysqladmin", "ping", "-h127.0.0.1", "-u" + opts.username};
    if (!opts.password.empty()) {
        argv.push_back("-p" + opts.password);
    }
    return argv;
}

std::vector<std::string> mariadb_ready_probe(const MySqlFamilyOptions&) {
    // The probe the image itself ships and documents: credential-free (the
    // entrypoint provisions a healthcheck user with materials in the
    // datadir), --connect forces TCP (fails during the bootstrap phase),
    // --innodb_initialized rejects the almost-up window. Also sidesteps the
    // image's renamed client binaries entirely.
    return {"healthcheck.sh", "--connect", "--innodb_initialized"};
}

} // namespace

const MySqlFamilyFlavor& mysql_flavor() {
    static const MySqlFamilyFlavor flavor{
        "MySQL",
        "MYSQL_USER",
        "MYSQL_PASSWORD",
        "MYSQL_ROOT_PASSWORD",
        "MYSQL_ALLOW_EMPTY_PASSWORD",
        "MYSQL_DATABASE",
        &mysql_ready_probe,
    };
    return flavor;
}

const MySqlFamilyFlavor& mariadb_flavor() {
    static const MySqlFamilyFlavor flavor{
        "MariaDB",
        "MARIADB_USER",
        "MARIADB_PASSWORD",
        "MARIADB_ROOT_PASSWORD",
        "MARIADB_ALLOW_EMPTY_ROOT_PASSWORD",
        "MARIADB_DATABASE",
        &mariadb_ready_probe,
    };
    return flavor;
}

bool is_root_username(const std::string& username) {
    if (username.size() != 4) {
        return false;
    }
    const std::string_view root = "root";
    for (std::size_t i = 0; i < 4; ++i) {
        if (std::tolower(static_cast<unsigned char>(username[i])) != root[i]) {
            return false;
        }
    }
    return true;
}

void add_init_script(MySqlFamilyOptions& opts, std::filesystem::path host_path) {
    const std::string name = host_path.filename().string();
    check_init_name(opts, name);
    const std::string target =
        "/docker-entrypoint-initdb.d/" + indexed_name(opts.init_scripts.size(), name);
    stage_init_copy(opts, name, CopyToContainer::host_file(std::move(host_path), target));
}

void add_init_script(MySqlFamilyOptions& opts, const std::string& name, std::string content) {
    if (name.empty() || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        throw Error("init script name \"" + name + "\" must be a bare file name");
    }
    check_init_name(opts, name);
    const std::string target =
        "/docker-entrypoint-initdb.d/" + indexed_name(opts.init_scripts.size(), name);
    stage_init_copy(opts, name, CopyToContainer::content(std::move(content), target));
}

void add_config_file(MySqlFamilyOptions& opts, std::filesystem::path host_cnf) {
    const std::string name = host_cnf.filename().string();
    if (name.size() <= 4 || !std::string_view(name).ends_with(".cnf")) {
        throw Error("config file \"" + name +
                    "\" must be named *.cnf — the image's /etc/mysql/conf.d include glob "
                    "would silently skip it");
    }
    // Default mode 0644 stands: the server IGNORES world-writable config files.
    opts.config_files.push_back(
        CopyToContainer::host_file(std::move(host_cnf), "/etc/mysql/conf.d/" + name));
}

GenericImage render(const MySqlFamilyFlavor& flavor, const GenericImage& base,
                    const MySqlFamilyOptions& opts) {
    // Fail fast, before any daemon contact: the images' own failure mode for
    // an undecided credential matrix is an entrypoint error message followed
    // by the full wait-timeout budget.
    if (opts.username.empty()) {
        throw Error(std::string(flavor.display_name) +
                    " username must not be empty (with_username)");
    }
    const bool root_only = is_root_username(opts.username);
    if (!root_only && opts.password.empty()) {
        throw Error(std::string(flavor.display_name) +
                    " refuses to create a user with an empty password: set with_password, or "
                    "use with_username(\"root\") for a passwordless root server");
    }

    // Render into a COPY: repeated to_generic()/start() calls must never
    // accumulate state in the config.
    GenericImage generic = base;

    // The managed credential matrix is appended AFTER any pass-through env
    // (both entrypoints are bash — the last duplicate wins), so the getters,
    // the DSN, and the probe cannot desynchronize from the server. Root-only
    // mode deliberately emits no *_USER key: both images refuse user=root.
    if (root_only) {
        if (!opts.password.empty()) {
            generic.with_env(flavor.env_root_password, opts.password);
        } else {
            generic.with_env(flavor.env_allow_empty_root, "yes");
        }
    } else {
        generic.with_env(flavor.env_user, opts.username);
        generic.with_env(flavor.env_password, opts.password);
        // Root shares the user's password by design: every boot carries a
        // root decision, and tests always hold a known superuser.
        generic.with_env(flavor.env_root_password, opts.password);
    }
    if (!opts.database.empty()) {
        generic.with_env(flavor.env_database, opts.database);
    }

    for (const CopyToContainer& script : opts.init_scripts) {
        generic.with_copy_to(script);
    }
    for (const CopyToContainer& cnf : opts.config_files) {
        generic.with_copy_to(cnf);
    }

    if (!opts.command_args.empty()) {
        // Arguments starting with '-' make the entrypoint exec the server
        // binary with them appended — no argv[0] needed.
        generic.with_cmd(opts.command_args);
    }

    if (opts.waits.empty()) {
        // First boot runs in two acts: a TEMPORARY bootstrap server bound to
        // the unix socket only (--skip-networking) initializes the datadir,
        // provisions credentials, and runs /docker-entrypoint-initdb.d; then
        // the real server starts on TCP 3306. The log line appears for BOTH
        // acts and a socket probe reads ready against the bootstrap instance
        // — only a TCP-forced probe proves the real server, and with it that
        // every init script already finished. 500ms poll (not the 200ms
        // default): each attempt is a fresh exec connection, pure daemon
        // churn against a boot measured in tens of seconds.
        wait_for::Command probe;
        probe.cmd = flavor.ready_probe(opts);
        probe.poll_interval = std::chrono::milliseconds(500);
        generic.with_wait(probe);
    } else {
        for (const WaitFor& wait : opts.waits) {
            generic.with_wait(wait);
        }
    }

    // Customizers last: what the escape hatch sets wins over the module's
    // rendering, mirroring how with_create_body_patch wins over typed fields.
    for (const auto& customize : opts.customizers) {
        customize(generic);
    }
    return generic;
}

std::string family_connection_string(const std::string& username, const std::string& password,
                                     const std::string& host, std::uint16_t port,
                                     const std::string& database) {
    ConnectionString url("mysql");
    url.with_user(username);
    if (!password.empty()) {
        url.with_password(password);
    }
    url.with_host(host).with_port(port);
    if (!database.empty()) {
        url.with_database(database);
    }
    return url.to_string();
}

} // namespace testcontainers::modules::detail
