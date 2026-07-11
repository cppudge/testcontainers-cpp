#include "testcontainers/modules/PostgreSQLContainer.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testcontainers/ConnectionString.hpp"
#include "testcontainers/Error.hpp"

namespace testcontainers::modules {

namespace {

/// The extensions the official entrypoint executes; anything else it SKIPS
/// silently — the worst failure mode (a green start, an empty schema) — so
/// the module refuses unknown ones up front.
constexpr std::string_view kInitExtensions[] = {".sql", ".sql.gz", ".sql.xz", ".sql.zst", ".sh"};

bool has_known_init_extension(std::string_view name) {
    for (const std::string_view ext : kInitExtensions) {
        if (name.size() > ext.size() && name.ends_with(ext)) {
            return true;
        }
    }
    return false;
}

/// "0000-name" — the entrypoint runs initdb.d files in C-collation name
/// order, so a zero-padded registration index makes registration order the
/// execution order. Four digits bound the scheme at 10000 scripts (the
/// registration guards throw before the index could outgrow them).
std::string indexed_name(std::size_t index, const std::string& name) {
    std::string digits = std::to_string(index);
    if (digits.size() < 4) {
        digits.insert(0, 4 - digits.size(), '0');
    }
    return digits + "-" + name;
}

/// libpq keyword/value quoting: values with whitespace (libpq's parser stops
/// an unquoted value at ANY isspace character), quotes, backslashes — or
/// empty ones — are single-quoted with ' and \ backslash-escaped.
std::string quote_conninfo_value(const std::string& value) {
    bool needs_quotes = value.empty();
    for (const char c : value) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' ||
            c == '\'' || c == '\\') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return value;
    }
    std::string quoted = "'";
    for (const char c : value) {
        if (c == '\'' || c == '\\') {
            quoted += '\\';
        }
        quoted += c;
    }
    quoted += '\'';
    return quoted;
}

} // namespace

PostgreSQLContainer::PostgreSQLContainer()
    : image_(GenericImage::from_reference(std::string(kDefaultImage))) {
    // Baked once. The exec probe forces TCP ("-h 127.0.0.1") on purpose: the
    // image's first boot runs a TEMPORARY server on the unix socket only
    // (initdb + init scripts), shuts it down, then starts the real server —
    // the only one that ever listens on TCP. A socket probe (or the "ready to
    // accept connections" log line, printed by BOTH servers) reads ready
    // inside that window; the TCP probe cannot, and it additionally proves
    // every init script finished. The -U/-d argv is interpolated at render
    // time (the credentials are not known yet here).
    image_.with_exposed_port(tcp(kPort));
}

PostgreSQLContainer& PostgreSQLContainer::with_image(const std::string& reference) {
    image_.with_image(reference);
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_username(std::string username) {
    username_ = std::move(username);
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_password(std::string password) {
    password_ = std::move(password);
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_database(std::string database) {
    database_ = std::move(database);
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_init_script(std::filesystem::path host_path) {
    const std::string name = host_path.filename().string();
    if (!has_known_init_extension(name)) {
        throw Error("init script \"" + name +
                    "\" has an extension the postgres entrypoint would silently ignore; "
                    "use .sql, .sql.gz, .sql.xz, .sql.zst, or .sh");
    }
    if (init_scripts_.size() >= 10000) {
        throw Error("too many init scripts (the 0000- ordering prefix is four digits)");
    }
    const std::string target =
        "/docker-entrypoint-initdb.d/" + indexed_name(init_scripts_.size(), name);
    CopyToContainer copy = CopyToContainer::host_file(std::move(host_path), target);
    if (name.ends_with(".sh")) {
        // Executable, so the entrypoint runs it as a standalone script; a
        // non-executable .sh is SOURCED into the entrypoint's shell instead
        // (where a stray `exit` kills the boot).
        copy.with_mode(0755);
    }
    init_scripts_.push_back(std::move(copy));
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_init_script(const std::string& name,
                                                           std::string content) {
    if (name.empty() || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        throw Error("init script name \"" + name + "\" must be a bare file name");
    }
    if (!has_known_init_extension(name)) {
        throw Error("init script \"" + name +
                    "\" has an extension the postgres entrypoint would silently ignore; "
                    "use .sql, .sql.gz, .sql.xz, .sql.zst, or .sh");
    }
    if (init_scripts_.size() >= 10000) {
        throw Error("too many init scripts (the 0000- ordering prefix is four digits)");
    }
    const std::string target =
        "/docker-entrypoint-initdb.d/" + indexed_name(init_scripts_.size(), name);
    CopyToContainer copy = CopyToContainer::content(std::move(content), target);
    if (name.ends_with(".sh")) {
        copy.with_mode(0755); // see the host-file overload
    }
    init_scripts_.push_back(std::move(copy));
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_config_option(std::string key, std::string value) {
    config_options_.emplace_back(std::move(key), std::move(value));
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_env(std::string key, std::string value) {
    image_.with_env(std::move(key), std::move(value));
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_label(std::string key, std::string value) {
    image_.with_label(std::move(key), std::move(value));
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_network(std::string network) {
    image_.with_network(std::move(network));
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_network(const Network& network) {
    image_.with_network(network);
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_network_alias(std::string alias) {
    image_.with_network_alias(std::move(alias));
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_reuse(bool reuse) {
    image_.with_reuse(reuse);
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_wait(WaitFor wait) {
    waits_.push_back(std::move(wait));
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_startup_timeout(std::chrono::milliseconds timeout) {
    image_.with_startup_timeout(timeout);
    return *this;
}

PostgreSQLContainer& PostgreSQLContainer::with_startup_attempts(int n) {
    image_.with_startup_attempts(n);
    return *this;
}

PostgreSQLContainer&
PostgreSQLContainer::with_customizer(std::function<void(GenericImage&)> customize) {
    customizers_.push_back(std::move(customize));
    return *this;
}

GenericImage PostgreSQLContainer::to_generic() const {
    // Fail fast, before any daemon contact: the image's own failure mode for
    // these is a log line followed by the full wait timeout.
    if (username_.empty()) {
        throw Error("PostgreSQL username must not be empty (with_username)");
    }
    if (database_.empty()) {
        throw Error("PostgreSQL database name must not be empty (with_database)");
    }
    if (password_.empty()) {
        // Last occurrence wins, matching how the image's bash entrypoint
        // resolves the Env list.
        std::string auth_method;
        for (const auto& [key, value] : image_.env()) {
            if (key == "POSTGRES_HOST_AUTH_METHOD") {
                auth_method = value;
            }
        }
        if (auth_method != "trust") {
            throw Error("the postgres image refuses to boot with an empty POSTGRES_PASSWORD: "
                        "set with_password, or opt into a passwordless server explicitly with "
                        "with_env(\"POSTGRES_HOST_AUTH_METHOD\", \"trust\")");
        }
    }

    // Render into a COPY: repeated to_generic()/start() calls must never
    // accumulate state in the config.
    GenericImage generic = image_;

    // The credential trio is appended AFTER any pass-through env (the image's
    // bash entrypoint applies the last duplicate in the Env list), so the
    // getters, the DSNs, and the probe's -U/-d can never desynchronize from
    // the server. An empty password (trust mode, validated above) appends no
    // POSTGRES_PASSWORD.
    generic.with_env("POSTGRES_USER", username_);
    if (!password_.empty()) {
        generic.with_env("POSTGRES_PASSWORD", password_);
    }
    generic.with_env("POSTGRES_DB", database_);

    if (!config_options_.empty()) {
        // The embedded builder never carries a cmd of its own (the module has
        // no with_cmd pass-through; a customizer's cmd lands later and wins),
        // so the rendered command always starts from the entrypoint's default
        // server argv[0].
        std::vector<std::string> cmd{"postgres"};
        for (const auto& [key, value] : config_options_) {
            cmd.emplace_back("-c");
            cmd.push_back(key + "=" + value);
        }
        generic.with_cmd(std::move(cmd));
    }

    for (const CopyToContainer& script : init_scripts_) {
        generic.with_copy_to(script);
    }

    if (waits_.empty()) {
        // See the constructor comment for why the probe forces TCP.
        generic.with_wait(wait_for::successful_command({"pg_isready", "-h", "127.0.0.1", "-p",
                                                        std::to_string(kPort), "-U", username_,
                                                        "-d", database_}));
    } else {
        for (const WaitFor& wait : waits_) {
            generic.with_wait(wait);
        }
    }

    // Customizers last: what the escape hatch sets wins over the module's
    // rendering, mirroring how with_create_body_patch wins over typed fields.
    for (const auto& customize : customizers_) {
        customize(generic);
    }
    return generic;
}

StartedPostgreSQL PostgreSQLContainer::start() const {
    return StartedPostgreSQL(to_generic().start(), username_, password_, database_);
}

std::string StartedPostgreSQL::connection_string(const std::string& scheme) const {
    ConnectionString url(scheme);
    url.with_user(username_);
    if (!password_.empty()) {
        url.with_password(password_);
    }
    url.with_host(host_).with_port(port_).with_database(database_);
    return url.to_string();
}

std::string StartedPostgreSQL::conninfo() const {
    std::string info = "host=" + quote_conninfo_value(host_) + " port=" + std::to_string(port_) +
                       " dbname=" + quote_conninfo_value(database_) +
                       " user=" + quote_conninfo_value(username_);
    if (!password_.empty()) {
        info += " password=" + quote_conninfo_value(password_);
    }
    return info;
}

ExecResult StartedPostgreSQL::exec_sql(const std::string& sql) const {
    // -X skips any ~/.psqlrc, -tA prints unaligned tuples only; the local
    // socket connection needs no password (the image's pg_hba trusts local).
    return container_.exec({"psql", "-v", "ON_ERROR_STOP=1", "-X", "-tA", "-U", username_, "-d",
                            database_, "-c", sql});
}

} // namespace testcontainers::modules
