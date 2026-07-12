#include "testcontainers/modules/ClickHouse.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ModuleDetail.hpp"
#include "testcontainers/ConnectionString.hpp"
#include "testcontainers/Error.hpp"

namespace testcontainers::modules {

namespace {

/// What the clickhouse entrypoint executes from initdb.d — narrower than the
/// postgres/mysql set (no xz/zst forms; those it would skip silently).
const std::vector<std::string_view>& clickhouse_init_extensions() {
    static const std::vector<std::string_view> extensions{".sql", ".sql.gz", ".sh"};
    return extensions;
}

} // namespace

ClickHouseImage::ClickHouseImage()
    : image_(GenericImage::from_reference(std::string(kDefaultImage))) {
    // Baked once. The readiness probe is rendered at to_generic() (so
    // with_wait can replace it); only the published ports live here.
    image_.with_exposed_port(tcp(kHttpPort)).with_exposed_port(tcp(kNativePort));
}

ClickHouseImage& ClickHouseImage::with_image(const std::string& reference) {
    image_.with_image(reference);
    return *this;
}

ClickHouseImage& ClickHouseImage::with_username(std::string username) {
    username_ = std::move(username);
    return *this;
}

ClickHouseImage& ClickHouseImage::with_password(std::string password) {
    password_ = std::move(password);
    return *this;
}

ClickHouseImage& ClickHouseImage::with_database(std::string database) {
    database_ = std::move(database);
    return *this;
}

ClickHouseImage& ClickHouseImage::with_init_script(std::filesystem::path host_path) {
    init_scripts_.push_back(detail::stage_init_script(std::move(host_path), init_scripts_.size(),
                                                      "the clickhouse entrypoint",
                                                      clickhouse_init_extensions()));
    return *this;
}

ClickHouseImage& ClickHouseImage::with_init_script(const std::string& name, std::string content) {
    init_scripts_.push_back(
        detail::stage_init_script(name, std::move(content), init_scripts_.size(),
                                  "the clickhouse entrypoint", clickhouse_init_extensions()));
    return *this;
}

ClickHouseImage& ClickHouseImage::with_config_file(std::filesystem::path host_config) {
    const std::string name = host_config.filename().string();
    const auto has_extension = [name_view = std::string_view(name)](std::string_view ext) {
        return name_view.size() > ext.size() && name_view.ends_with(ext);
    };
    if (!has_extension(".xml") && !has_extension(".yaml") && !has_extension(".yml")) {
        throw Error("config drop-in \"" + name +
                    "\" must be named *.xml, *.yaml, or *.yml — the server merges only those "
                    "from config.d and would silently skip it");
    }
    config_files_.push_back(CopyToContainer::host_file(std::move(host_config),
                                                       "/etc/clickhouse-server/config.d/" + name));
    return *this;
}

ClickHouseImage& ClickHouseImage::with_env(std::string key, std::string value) {
    image_.with_env(std::move(key), std::move(value));
    return *this;
}

ClickHouseImage& ClickHouseImage::with_label(std::string key, std::string value) {
    image_.with_label(std::move(key), std::move(value));
    return *this;
}

ClickHouseImage& ClickHouseImage::with_network(std::string network) {
    image_.with_network(std::move(network));
    return *this;
}

ClickHouseImage& ClickHouseImage::with_network(const Network& network) {
    image_.with_network(network);
    return *this;
}

ClickHouseImage& ClickHouseImage::with_network_alias(std::string alias) {
    image_.with_network_alias(std::move(alias));
    return *this;
}

ClickHouseImage& ClickHouseImage::with_reuse(bool reuse) {
    image_.with_reuse(reuse);
    return *this;
}

ClickHouseImage& ClickHouseImage::with_wait(WaitFor wait) {
    waits_.push_back(std::move(wait));
    return *this;
}

ClickHouseImage& ClickHouseImage::with_startup_timeout(std::chrono::milliseconds timeout) {
    image_.with_startup_timeout(timeout);
    return *this;
}

ClickHouseImage& ClickHouseImage::with_startup_attempts(int n) {
    image_.with_startup_attempts(n);
    return *this;
}

ClickHouseImage& ClickHouseImage::with_customizer(std::function<void(GenericImage&)> customize) {
    customizers_.push_back(std::move(customize));
    return *this;
}

GenericImage ClickHouseImage::to_generic() const {
    // Fail fast, before any daemon contact.
    if (username_.empty()) {
        throw Error("ClickHouse username must not be empty (with_username)");
    }
    if (database_.empty()) {
        throw Error("ClickHouse database name must not be empty (with_database)");
    }
    if (password_.empty()) {
        // Deliberate throw-always: with an empty password the image falls
        // back to its built-in `default` user and restricts it to the
        // container's loopback — every host-side getter of this module would
        // lie. Deliberately open servers drop to to_generic() on a plain
        // GenericImage instead.
        throw Error("ClickHouse password must not be empty (with_password): the image "
                    "restricts a passwordless server to the container's loopback, which the "
                    "module's host-side getters cannot describe — configure a password, or "
                    "build the container from a plain GenericImage");
    }

    // Render into a COPY: repeated to_generic()/start() calls must never
    // accumulate state in the config.
    GenericImage generic = image_;

    // The credential trio is appended AFTER any pass-through env (the image's
    // bash entrypoint applies the last duplicate in the Env list), so the
    // getters, the DSN, and exec_sql can never desynchronize from the server.
    generic.with_env("CLICKHOUSE_USER", username_);
    generic.with_env("CLICKHOUSE_PASSWORD", password_);
    generic.with_env("CLICKHOUSE_DB", database_);

    for (const CopyToContainer& script : init_scripts_) {
        generic.with_copy_to(script);
    }
    for (const CopyToContainer& config : config_files_) {
        generic.with_copy_to(config);
    }

    if (waits_.empty()) {
        // ORDERED triple — each leg is load-bearing. (1) The image's first
        // boot runs a TEMPORARY server for provisioning and init scripts;
        // network probes cannot tell it from the real one (it binds the
        // container's loopback, but Docker Desktop's port proxy reaches
        // loopback-bound listeners too — observed live: /ping answered 200
        // out of the init window). What does tell them apart: the entrypoint
        // finishes init by EXEC'ing the real server over itself, flipping
        // /proc/1/comm from "entrypoint.sh" to "clickhouse-serv" — past this
        // probe the temporary server is gone and every init script finished
        // (a failing script aborts the boot instead). (2) /ping through the
        // PUBLISHED port, status-checked: end-to-end proof of the mapping —
        // now guaranteed to be the real server. (3) An in-container SELECT 1
        // over the native protocol: the server opens 8123 a beat before
        // 9000 (an API exec is fast enough to hit that gap), and this also
        // proves the provisioned credentials.
        generic.with_wait(wait_for::successful_shell_command("grep -q clickhouse /proc/1/comm"));
        generic.with_wait(wait_for::http("/ping", tcp(kHttpPort)));
        generic.with_wait(wait_for::successful_command({"clickhouse-client", "--user", username_,
                                                        "--password", password_, "--database",
                                                        database_, "--query", "SELECT 1"}));
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

ClickHouseContainer ClickHouseImage::start() const {
    return ClickHouseContainer(to_generic().start(), username_, password_, database_);
}

std::string ClickHouseContainer::connection_string() const {
    ConnectionString url("clickhouse");
    url.with_user(username_).with_password(password_).with_host(host_).with_port(native_port_);
    url.with_database(database_);
    return url.to_string();
}

std::string ClickHouseContainer::http_url() const {
    ConnectionString url("http");
    url.with_host(host_).with_port(http_port_);
    return url.to_string();
}

ExecResult ClickHouseContainer::exec_sql(const std::string& sql) const {
    // Native protocol over the container's loopback — by the time start()
    // returned, the readiness triple proved the real server's native listener
    // (the temporary provisioning server is long gone). One statement per
    // call; output is the client's non-interactive TabSeparated default.
    return container_.exec({"clickhouse-client", "--user", username_, "--password", password_,
                            "--database", database_, "--query", sql});
}

} // namespace testcontainers::modules
