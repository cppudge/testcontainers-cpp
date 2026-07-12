#include "testcontainers/modules/Redis.hpp"

#include <string>
#include <utility>
#include <vector>

#include "testcontainers/ConnectionString.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/WaitFor.hpp"

namespace testcontainers::modules {

RedisImage::RedisImage() : image_(GenericImage::from_reference(std::string(kDefaultImage))) {
    // Baked once, never touched by rendering. An exec probe rather than a log
    // or TCP wait: the "Ready to accept connections" log line races the
    // listener, and Docker Desktop's host proxy accepts on a published port
    // even when nothing listens behind it — `redis-cli ping` proves the
    // server ANSWERS. REDISCLI_AUTH (set by with_password at render time)
    // keeps the same probe valid with and without auth.
    image_.with_exposed_port(tcp(kPort))
        .with_wait(wait_for::successful_command({"redis-cli", "ping"}));
}

RedisImage& RedisImage::with_image(const std::string& reference) {
    image_.with_image(reference);
    return *this;
}

RedisImage& RedisImage::with_password(std::string password) {
    password_ = std::move(password);
    return *this;
}

RedisImage& RedisImage::with_command_args(std::vector<std::string> args) {
    for (std::string& arg : args) {
        args_.push_back(std::move(arg));
    }
    return *this;
}

RedisImage& RedisImage::with_command_arg(std::string arg) {
    args_.push_back(std::move(arg));
    return *this;
}

RedisImage& RedisImage::with_env(std::string key, std::string value) {
    image_.with_env(std::move(key), std::move(value));
    return *this;
}

RedisImage& RedisImage::with_label(std::string key, std::string value) {
    image_.with_label(std::move(key), std::move(value));
    return *this;
}

RedisImage& RedisImage::with_network(std::string network) {
    image_.with_network(std::move(network));
    return *this;
}

RedisImage& RedisImage::with_network(const Network& network) {
    image_.with_network(network);
    return *this;
}

RedisImage& RedisImage::with_network_alias(std::string alias) {
    image_.with_network_alias(std::move(alias));
    return *this;
}

RedisImage& RedisImage::with_reuse(bool reuse) {
    image_.with_reuse(reuse);
    return *this;
}

RedisImage& RedisImage::with_startup_timeout(std::chrono::milliseconds timeout) {
    image_.with_startup_timeout(timeout);
    return *this;
}

RedisImage& RedisImage::with_startup_attempts(int n) {
    image_.with_startup_attempts(n);
    return *this;
}

RedisImage& RedisImage::with_customizer(std::function<void(GenericImage&)> customize) {
    customizers_.push_back(std::move(customize));
    return *this;
}

GenericImage RedisImage::to_generic() const {
    if (!password_.empty()) {
        // Fail fast: REDISCLI_AUTH is read by EXEC'D processes (the probe's
        // redis-cli, user execs), where glibc getenv returns the FIRST
        // duplicate of a key — a raw with_env entry would beat the module's
        // appended copy and desync the probe from --requirepass.
        for (const auto& [key, value] : image_.env()) {
            if (key == "REDISCLI_AUTH") {
                throw Error(
                    "REDISCLI_AUTH is managed by with_password: a raw with_env entry would win "
                    "over the module's in exec'd processes (the first duplicate wins there) and "
                    "desync the readiness probe — drop the with_env call");
            }
        }
    }

    // Render into a COPY: repeated to_generic()/start() calls must never
    // accumulate state in the config.
    GenericImage generic = image_;
    if (!password_.empty() || !args_.empty()) {
        // argv[0] stays "redis-server": the official image's entrypoint keys
        // its protected-mode handling off it (a password-less server would
        // otherwise refuse the connections arriving through the port mapping).
        std::vector<std::string> cmd{"redis-server"};
        if (!password_.empty()) {
            cmd.insert(cmd.end(), {"--requirepass", password_});
        }
        cmd.insert(cmd.end(), args_.begin(), args_.end());
        generic.with_cmd(std::move(cmd));
    }
    if (!password_.empty()) {
        // Container-level env: in-container redis-cli — the readiness probe
        // and user execs — reads it and authenticates without an -a flag on
        // its command line. The validation above guarantees this stays the
        // only occurrence (duplicate-key resolution is process-dependent:
        // bash entrypoints see the last occurrence, exec'd glibc processes
        // the first — redis-cli is the latter).
        generic.with_env("REDISCLI_AUTH", password_);
    }
    // Customizers last: what the escape hatch sets wins over the module's
    // rendering, mirroring how with_create_body_patch wins over typed fields.
    for (const auto& customize : customizers_) {
        customize(generic);
    }
    return generic;
}

RedisContainer RedisImage::start() const { return RedisContainer(to_generic().start(), password_); }

std::string RedisContainer::connection_string(int database) const {
    ConnectionString url("redis");
    if (!password_.empty()) {
        url.with_password(password_); // renders ":pass@" — the password-sans-user URI form
    }
    url.with_host(host()).with_port(port());
    if (database != 0) {
        url.with_database(std::to_string(database));
    }
    return url.to_string();
}

} // namespace testcontainers::modules
