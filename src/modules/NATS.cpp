#include "testcontainers/modules/NATS.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testcontainers/ConnectionString.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/WaitFor.hpp"

namespace testcontainers::modules {

namespace {

/// Flags the module renders, or whose effects its getters and waits bake in.
/// nats-server keeps the LAST occurrence of a duplicated flag, so — unlike
/// the DB modules' append-last env keys — no ordering can make the module's
/// copy win over a raw one: a duplicate silently desyncs url()/the getters
/// (--user/--pass, -js), starves a readiness probe or getter (-m, -p, -a), or
/// re-sets any of those invisibly (-c). Render throws instead.
constexpr std::string_view kManagedFlagNames[] = {
    "user", "pass",                          // with_username / with_password
    "js",   "jetstream",                     // with_jetstream
    "m",    "http_port",                     // monitoring listener: the /healthz wait's target
    "p",    "port",      "a", "addr", "net", // client listener: port()/url() resolve 4222
    "c",    "config",                        // a config file can re-set any of the above
};

/// True for `-name`, `--name`, and their `=value` forms; plain values (no
/// leading dash) never match.
bool is_managed_flag(std::string_view arg) {
    if (arg.size() < 2 || arg[0] != '-') {
        return false;
    }
    arg.remove_prefix(arg[1] == '-' ? 2 : 1);
    if (const std::size_t eq = arg.find('='); eq != std::string_view::npos) {
        arg = arg.substr(0, eq);
    }
    for (const std::string_view name : kManagedFlagNames) {
        if (arg == name) {
            return true;
        }
    }
    return false;
}

} // namespace

NATSImage::NATSImage() : image_(GenericImage::from_reference(std::string(kDefaultImage))) {
    // Baked once, never touched by rendering. ORDERED waits: the log line
    // proves the server process came up (nats-server logs to stderr; the
    // either-stream wait survives an upstream logging change); the /healthz
    // probe then proves it end to end through the published port — Docker
    // Desktop's host proxy accepts TCP before the backend exists, but an
    // HTTP 200 cannot be faked. With -js the same endpoint also gates
    // JetStream health.
    image_.with_exposed_port(tcp(kClientPort))
        .with_exposed_port(tcp(kMonitoringPort))
        .with_wait(wait_for::log("Server is ready"))
        .with_wait(wait_for::http("/healthz", tcp(kMonitoringPort)));
}

NATSImage& NATSImage::with_image(const std::string& reference) {
    image_.with_image(reference);
    return *this;
}

NATSImage& NATSImage::with_username(std::string username) {
    username_ = std::move(username);
    return *this;
}

NATSImage& NATSImage::with_password(std::string password) {
    password_ = std::move(password);
    return *this;
}

NATSImage& NATSImage::with_jetstream(bool enable) {
    jetstream_ = enable;
    return *this;
}

NATSImage& NATSImage::with_command_args(std::vector<std::string> args) {
    for (std::string& arg : args) {
        args_.push_back(std::move(arg));
    }
    return *this;
}

NATSImage& NATSImage::with_command_arg(std::string arg) {
    args_.push_back(std::move(arg));
    return *this;
}

NATSImage& NATSImage::with_env(std::string key, std::string value) {
    image_.with_env(std::move(key), std::move(value));
    return *this;
}

NATSImage& NATSImage::with_label(std::string key, std::string value) {
    image_.with_label(std::move(key), std::move(value));
    return *this;
}

NATSImage& NATSImage::with_network(std::string network) {
    image_.with_network(std::move(network));
    return *this;
}

NATSImage& NATSImage::with_network(const Network& network) {
    image_.with_network(network);
    return *this;
}

NATSImage& NATSImage::with_network_alias(std::string alias) {
    image_.with_network_alias(std::move(alias));
    return *this;
}

NATSImage& NATSImage::with_reuse(bool reuse) {
    image_.with_reuse(reuse);
    return *this;
}

NATSImage& NATSImage::with_startup_timeout(std::chrono::milliseconds timeout) {
    image_.with_startup_timeout(timeout);
    return *this;
}

NATSImage& NATSImage::with_startup_attempts(int n) {
    image_.with_startup_attempts(n);
    return *this;
}

NATSImage& NATSImage::with_customizer(std::function<void(GenericImage&)> customize) {
    customizers_.push_back(std::move(customize));
    return *this;
}

GenericImage NATSImage::to_generic() const {
    // Fail fast, before any daemon contact.
    if (username_.empty() != password_.empty()) {
        throw Error("NATS credentials come as a pair: set both with_username and "
                    "with_password, or neither");
    }
    for (const std::string& arg : args_) {
        if (is_managed_flag(arg)) {
            throw Error("nats-server flag \"" + arg +
                        "\" is managed by the module: the server keeps the LAST duplicate of "
                        "a flag, so a raw copy would win and desync the connection getters or "
                        "the readiness probes — use the typed setter (with_username / "
                        "with_password / with_jetstream), or take full command ownership with "
                        "a customizer's with_cmd");
        }
    }

    // Render into a COPY: repeated to_generic()/start() calls must never
    // accumulate state in the config.
    GenericImage generic = image_;
    // The module always owns the command. Any override drops the image's
    // default config file (its whole content is the stock client/monitoring
    // ports), so monitoring is restated unconditionally: one code path, and
    // monitoring_url() holds on every image variant. The leading dash also
    // keeps the -alpine entrypoint routing the args to nats-server.
    std::vector<std::string> cmd{"-m", std::to_string(kMonitoringPort)};
    if (!username_.empty()) {
        cmd.insert(cmd.end(), {"--user", username_, "--pass", password_});
    }
    if (jetstream_) {
        cmd.emplace_back("-js");
    }
    cmd.insert(cmd.end(), args_.begin(), args_.end());
    generic.with_cmd(std::move(cmd));

    // Customizers last: what the escape hatch sets wins over the module's
    // rendering, mirroring how with_create_body_patch wins over typed fields.
    for (const auto& customize : customizers_) {
        customize(generic);
    }
    return generic;
}

NATSContainer NATSImage::start() const {
    return NATSContainer(to_generic().start(), username_, password_);
}

std::string NATSContainer::url() const {
    ConnectionString url("nats");
    if (!username_.empty()) {
        url.with_user(username_).with_password(password_);
    }
    url.with_host(host_).with_port(port_);
    return url.to_string();
}

std::string NATSContainer::monitoring_url() const {
    ConnectionString url("http");
    url.with_host(host_).with_port(monitoring_port_);
    return url.to_string();
}

} // namespace testcontainers::modules
