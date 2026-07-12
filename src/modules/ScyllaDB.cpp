#include "testcontainers/modules/ScyllaDB.hpp"

#include <string>
#include <utility>
#include <vector>

#include "ModuleDetail.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers::modules {

namespace {

/// cqlsh addressing used everywhere in this module: the entrypoint binds CQL
/// to the CONTAINER IP, not 127.0.0.1, and the bundled cqlsh's default host
/// has regressed against that before (scylladb/scylladb#16329) — resolving
/// `$(hostname)` in-shell is regression-proof. The CQL/file argument rides
/// as a positional shell parameter, so no escaping of quotes is ever needed.
std::vector<std::string> cqlsh_exec(const char* flag, const std::string& argument) {
    return {"sh", "-c", std::string("cqlsh \"$(hostname)\" ") + flag + " \"$1\"", "cqlsh",
            argument};
}

/// Stage one post-ready .cql script under /tmp (the image has no initdb.d;
/// the parent directory must already exist for a single-file copy). The
/// zero-padded registration prefix keeps the on-disk names ordered like the
/// hook's execution order, so a `keep()`ed container reads naturally.
std::string init_target(std::size_t index, const std::string& name) {
    if (index >= 10000) {
        throw Error("too many init scripts (the 0000- ordering prefix is four digits)");
    }
    if (name.size() <= 4 || !name.ends_with(".cql")) {
        throw Error("init script '" + name +
                    "' must be named *.cql — the module runs scripts through cqlsh -f");
    }
    return "/tmp/testcontainers-" + detail::zero_pad4(index) + "-" + name;
}

} // namespace

ScyllaDBImage::ScyllaDBImage() : image_(GenericImage::from_reference(std::string(kDefaultImage))) {
    // Baked once: the CQL port, and a budget over the core default — a first
    // boot initializes the data directory and routinely takes tens of
    // seconds on loaded CI (observed seconds locally). The readiness pair is
    // rendered at to_generic() so with_wait can replace it.
    image_.with_exposed_port(tcp(kPort));
    image_.with_startup_timeout(std::chrono::seconds(120));
}

ScyllaDBImage& ScyllaDBImage::with_image(const std::string& reference) {
    image_.with_image(reference);
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_smp(int shards) {
    smp_ = shards;
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_memory(std::string amount) {
    memory_ = std::move(amount);
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_datacenter(std::string name) {
    datacenter_ = std::move(name);
    datacenter_set_ = true;
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_command_args(std::vector<std::string> args) {
    for (std::string& arg : args) {
        extra_args_.push_back(std::move(arg));
    }
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_command_arg(std::string arg) {
    extra_args_.push_back(std::move(arg));
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_init_script(std::filesystem::path host_path) {
    const std::string target = init_target(init_scripts_.size(), host_path.filename().string());
    init_scripts_.push_back(CopyToContainer::host_file(std::move(host_path), target));
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_init_script(const std::string& name, std::string content) {
    if (name.empty() || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        throw Error("init script name '" + name + "' must be a bare file name");
    }
    const std::string target = init_target(init_scripts_.size(), name);
    init_scripts_.push_back(CopyToContainer::content(std::move(content), target));
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_env(std::string key, std::string value) {
    image_.with_env(std::move(key), std::move(value));
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_label(std::string key, std::string value) {
    image_.with_label(std::move(key), std::move(value));
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_network(std::string network) {
    image_.with_network(std::move(network));
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_network(const Network& network) {
    image_.with_network(network);
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_network_alias(std::string alias) {
    image_.with_network_alias(std::move(alias));
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_reuse(bool reuse) {
    image_.with_reuse(reuse);
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_wait(WaitFor wait) {
    waits_.push_back(std::move(wait));
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_startup_timeout(std::chrono::milliseconds timeout) {
    image_.with_startup_timeout(timeout);
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_startup_attempts(int n) {
    image_.with_startup_attempts(n);
    return *this;
}

ScyllaDBImage& ScyllaDBImage::with_customizer(std::function<void(GenericImage&)> customize) {
    customizers_.push_back(std::move(customize));
    return *this;
}

GenericImage ScyllaDBImage::to_generic() const {
    // Fail fast, before any daemon contact — the server's own reaction to a
    // bad flag is an opaque boot failure surfacing as the full wait timeout.
    if (smp_ < 1) {
        throw Error("ScyllaDB shard count must be at least 1 (with_smp)");
    }
    if (memory_.empty()) {
        throw Error("ScyllaDB memory amount must not be empty (with_memory)");
    }
    if (datacenter_set_ && datacenter_.empty()) {
        throw Error("ScyllaDB datacenter name must not be empty (with_datacenter)");
    }

    // Render into a COPY: repeated to_generic()/start() calls must never
    // accumulate state in the config.
    GenericImage generic = image_;

    // Managed flags first, user args after: the entrypoint's parser keeps the
    // LAST occurrence of a flag, so a user duplicate wins (tuning knobs, no
    // credential getters to desync — the Kafka rule, not the DB-module one).
    std::vector<std::string> cmd{"--developer-mode=1",
                                 "--overprovisioned=1",
                                 "--smp",
                                 std::to_string(smp_),
                                 "--memory",
                                 memory_};
    if (datacenter_set_) {
        cmd.emplace_back("--dc");
        cmd.push_back(datacenter_);
    }
    cmd.insert(cmd.end(), extra_args_.begin(), extra_args_.end());
    generic.with_cmd(std::move(cmd));

    for (const CopyToContainer& script : init_scripts_) {
        generic.with_copy_to(script);
    }

    if (waits_.empty()) {
        // Ordered pair. (1) The CQL-listening line is printed only after
        // node init/bootstrap, gating out the long boot without burning
        // execs (the stable prefix — the suffix carries the address and
        // varies). (2) The cqlsh probe is authoritative: the query path
        // answers. A port-only wait is out — Docker Desktop's host proxy
        // accepts connections regardless of the server behind them.
        generic.with_wait(wait_for::log("Starting listening for CQL clients"));
        generic.with_wait(wait_for::successful_shell_command(
            "cqlsh \"$(hostname)\" -e \"SELECT release_version FROM system.local\""));
    } else {
        for (const WaitFor& wait : waits_) {
            generic.with_wait(wait);
        }
    }

    if (!init_scripts_.empty()) {
        // Post-ready seeding: the image has no initdb.d, so the scripts run
        // through cqlsh once the node answers. Inputs are the copy-to
        // descriptors above — already reuse-hash-covered, no label needed.
        std::vector<std::string> targets;
        targets.reserve(init_scripts_.size());
        for (const CopyToContainer& script : init_scripts_) {
            targets.push_back(script.target());
        }
        generic.with_started_hook([targets](DockerClient& client, const std::string& id) {
            for (const std::string& target : targets) {
                detail::exec_or_throw(client, id, cqlsh_exec("-f", target),
                                      "running scylladb init script '" + target + "'");
            }
        });
    }

    // Customizers last: what the escape hatch sets wins over the module's
    // rendering, mirroring how with_create_body_patch wins over typed fields.
    for (const auto& customize : customizers_) {
        customize(generic);
    }
    return generic;
}

ScyllaDBContainer ScyllaDBImage::start() const {
    return ScyllaDBContainer(to_generic().start(), datacenter_);
}

ExecResult ScyllaDBContainer::exec_cql(const std::string& cql) const {
    return container_.exec(cqlsh_exec("-e", cql));
}

} // namespace testcontainers::modules
