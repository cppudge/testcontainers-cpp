#include "docker/ApiMapping.hpp"

#include "testcontainers/Error.hpp"

#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

namespace testcontainers::docker {

namespace {

/// Cap a response body for inclusion in an error message: a misrouted proxy /
/// captive portal can hand back megabytes of HTML through a 200, and the
/// exception text must stay readable.
std::string body_excerpt(const std::string& body) {
    constexpr std::size_t kMax = 2048;
    if (body.size() <= kMax) {
        return body;
    }
    return body.substr(0, kMax) + "... [truncated, " + std::to_string(body.size()) +
           " bytes total]";
}

/// Run a parse function, wrapping ANY nlohmann failure (invalid JSON or an
/// unexpected shape/type) in a DockerError prefixed with `context`, so callers
/// see one uniform error type instead of raw nlohmann exceptions (an HTML
/// error page smuggled through a 200 must not escape as json::parse_error).
template <class Fn> auto guard_parse(const char* context, const std::string& body, Fn&& fn) {
    try {
        return fn();
    } catch (const nlohmann::json::exception& e) {
        throw DockerError(std::string(context) + ": unexpected response from Docker: " + e.what() +
                          "; body: " + body_excerpt(body));
    }
}

} // namespace

nlohmann::json build_create_body(const CreateContainerSpec& spec) {
    nlohmann::json body;
    body["Image"] = spec.image;

    if (!spec.cmd.empty()) {
        body["Cmd"] = spec.cmd;
    }
    if (!spec.entrypoint.empty()) {
        body["Entrypoint"] = spec.entrypoint;
    }
    if (!spec.env.empty()) {
        body["Env"] = spec.env;
    }
    if (spec.working_dir) {
        body["WorkingDir"] = *spec.working_dir;
    }
    if (spec.user) {
        body["User"] = *spec.user;
    }
    if (spec.tty) {
        body["Tty"] = true;
    }
    if (!spec.labels.empty()) {
        nlohmann::json labels = nlohmann::json::object();
        for (const auto& [key, value] : spec.labels) {
            labels[key] = value;
        }
        body["Labels"] = std::move(labels);
    }
    if (!spec.exposed_ports.empty()) {
        nlohmann::json exposed = nlohmann::json::object();
        for (const auto& port : spec.exposed_ports) {
            exposed[port] = nlohmann::json::object();
        }
        body["ExposedPorts"] = std::move(exposed);
    }

    if (spec.healthcheck) {
        const Healthcheck& hc = *spec.healthcheck;
        nlohmann::json health = nlohmann::json::object();
        health["Test"] = hc.test();
        if (hc.interval()) {
            health["Interval"] = static_cast<std::int64_t>(hc.interval()->count());
        }
        if (hc.timeout()) {
            health["Timeout"] = static_cast<std::int64_t>(hc.timeout()->count());
        }
        if (hc.start_period()) {
            health["StartPeriod"] = static_cast<std::int64_t>(hc.start_period()->count());
        }
        if (hc.retries()) {
            health["Retries"] = *hc.retries();
        }
        body["Healthcheck"] = std::move(health);
    }

    nlohmann::json host_config = nlohmann::json::object();
    if (spec.publish_all_ports) {
        host_config["PublishAllPorts"] = true;
    }
    if (spec.privileged) {
        host_config["Privileged"] = true;
    }
    if (spec.auto_remove) {
        host_config["AutoRemove"] = true;
    }
    if (spec.network) {
        host_config["NetworkMode"] = *spec.network;
    }
    if (spec.isolation) {
        host_config["Isolation"] = *spec.isolation;
    }
    if (spec.memory_bytes) {
        host_config["Memory"] = *spec.memory_bytes;
    }
    if (spec.shm_size_bytes) {
        host_config["ShmSize"] = *spec.shm_size_bytes;
    }
    if (!spec.ulimits.empty()) {
        nlohmann::json ulimits = nlohmann::json::array();
        for (const Ulimit& u : spec.ulimits) {
            nlohmann::json entry = nlohmann::json::object();
            entry["Name"] = u.name;
            entry["Soft"] = u.soft;
            entry["Hard"] = u.hard;
            ulimits.push_back(std::move(entry));
        }
        host_config["Ulimits"] = std::move(ulimits);
    }
    if (!spec.cap_add.empty()) {
        host_config["CapAdd"] = spec.cap_add;
    }
    if (!spec.cap_drop.empty()) {
        host_config["CapDrop"] = spec.cap_drop;
    }
    if (!spec.extra_hosts.empty()) {
        host_config["ExtraHosts"] = spec.extra_hosts;
    }
    if (!spec.mounts.empty()) {
        nlohmann::json mounts = nlohmann::json::array();
        for (const Mount& m : spec.mounts) {
            nlohmann::json entry = nlohmann::json::object();
            switch (m.type()) {
            case MountType::Bind:
                entry["Type"] = "bind";
                entry["Source"] = m.source();
                break;
            case MountType::Volume:
                entry["Type"] = "volume";
                entry["Source"] = m.source();
                break;
            case MountType::Tmpfs:
                entry["Type"] = "tmpfs";
                // No Source for tmpfs.
                break;
            }
            entry["Target"] = m.target();
            entry["ReadOnly"] = m.is_read_only();
            if (m.type() == MountType::Tmpfs && (m.tmpfs_size() || m.tmpfs_mode())) {
                nlohmann::json tmpfs = nlohmann::json::object();
                if (m.tmpfs_size()) {
                    tmpfs["SizeBytes"] = *m.tmpfs_size();
                }
                if (m.tmpfs_mode()) {
                    tmpfs["Mode"] = *m.tmpfs_mode();
                }
                entry["TmpfsOptions"] = std::move(tmpfs);
            }
            mounts.push_back(std::move(entry));
        }
        host_config["Mounts"] = std::move(mounts);
    }
    if (!host_config.empty()) {
        body["HostConfig"] = std::move(host_config);
    }

    // Network aliases require a target network to attach to; without one there is
    // nothing to alias, so they are ignored (no-op) when `network` is unset.
    if (spec.network && !spec.network_aliases.empty()) {
        nlohmann::json endpoint = nlohmann::json::object();
        endpoint["Aliases"] = spec.network_aliases;
        nlohmann::json endpoints = nlohmann::json::object();
        endpoints[*spec.network] = std::move(endpoint);
        nlohmann::json networking = nlohmann::json::object();
        networking["EndpointsConfig"] = std::move(endpoints);
        body["NetworkingConfig"] = std::move(networking);
    }

    if (!spec.create_body_patch.empty()) {
        // RFC 7386 deep-merge of a raw Docker create-body fragment (escape hatch).
        // Lets callers set any field (nest HostConfig fields under "HostConfig").
        try {
            body.merge_patch(nlohmann::json::parse(spec.create_body_patch));
        } catch (const nlohmann::json::parse_error& e) {
            throw DockerError(std::string("create_body_patch is not valid JSON: ") + e.what());
        }
    }

    return body;
}

nlohmann::json build_connect_network_body(const std::string& container_id,
                                          const std::vector<std::string>& aliases) {
    nlohmann::json body;
    body["Container"] = container_id;
    if (!aliases.empty()) {
        nlohmann::json endpoint = nlohmann::json::object();
        endpoint["Aliases"] = aliases;
        body["EndpointConfig"] = std::move(endpoint);
    }
    return body;
}

nlohmann::json build_network_create_body(const NetworkCreateSpec& spec) {
    nlohmann::json body;
    body["Name"] = spec.name;

    if (spec.driver) {
        body["Driver"] = *spec.driver;
    }
    if (spec.internal) {
        body["Internal"] = true;
    }
    if (spec.attachable) {
        body["Attachable"] = true;
    }
    if (spec.enable_ipv6) {
        body["EnableIPv6"] = true;
    }
    if (!spec.options.empty()) {
        nlohmann::json options = nlohmann::json::object();
        for (const auto& [key, value] : spec.options) {
            options[key] = value;
        }
        body["Options"] = std::move(options);
    }
    if (!spec.labels.empty()) {
        nlohmann::json labels = nlohmann::json::object();
        for (const auto& [key, value] : spec.labels) {
            labels[key] = value;
        }
        body["Labels"] = std::move(labels);
    }
    if (spec.subnet || spec.gateway) {
        nlohmann::json config = nlohmann::json::object();
        if (spec.subnet) {
            config["Subnet"] = *spec.subnet;
        }
        if (spec.gateway) {
            config["Gateway"] = *spec.gateway;
        }
        nlohmann::json ipam = nlohmann::json::object();
        ipam["Config"] = nlohmann::json::array({std::move(config)});
        body["IPAM"] = std::move(ipam);
    }

    return body;
}

nlohmann::json build_volume_create_body(const VolumeCreateSpec& spec) {
    nlohmann::json body;
    body["Name"] = spec.name;

    if (spec.driver) {
        body["Driver"] = *spec.driver;
    }
    if (!spec.driver_opts.empty()) {
        nlohmann::json opts = nlohmann::json::object();
        for (const auto& [key, value] : spec.driver_opts) {
            opts[key] = value;
        }
        body["DriverOpts"] = std::move(opts);
    }
    if (!spec.labels.empty()) {
        nlohmann::json labels = nlohmann::json::object();
        for (const auto& [key, value] : spec.labels) {
            labels[key] = value;
        }
        body["Labels"] = std::move(labels);
    }

    return body;
}

VolumeInspect parse_volume_inspect(const std::string& body) {
    return guard_parse("inspect_volume", body, [&] {
        const nlohmann::json json = nlohmann::json::parse(body);

        VolumeInspect info;
        info.name = json.value("Name", std::string{});
        info.driver = json.value("Driver", std::string{});
        info.mountpoint = json.value("Mountpoint", std::string{});
        info.scope = json.value("Scope", std::string{});

        if (const auto labels = json.find("Labels"); labels != json.end() && labels->is_object()) {
            for (const auto& [key, value] : labels->items()) {
                info.labels.emplace(key, value.get<std::string>());
            }
        }
        if (const auto options = json.find("Options");
            options != json.end() && options->is_object()) {
            for (const auto& [key, value] : options->items()) {
                info.options.emplace(key, value.get<std::string>());
            }
        }

        return info;
    });
}

std::string parse_server_os(const std::string& version_json) {
    return guard_parse("server_os (GET /version)", version_json, [&] {
        const nlohmann::json json = nlohmann::json::parse(version_json);
        return json.value("Os", std::string{});
    });
}

std::string build_create_query(const CreateContainerSpec& spec,
                               const std::function<std::string(const std::string&)>& encode) {
    std::string query;
    auto append = [&](const std::string& key, const std::string& value) {
        query += query.empty() ? '?' : '&';
        query += key;
        query += '=';
        query += encode(value);
    };
    if (spec.name) {
        append("name", *spec.name);
    }
    if (spec.platform) {
        // Free-form "<os>/<arch>", e.g. "windows/amd64".
        append("platform", *spec.platform);
    }
    return query;
}

static ContainerInspect parse_inspect_unguarded(const std::string& body);

ContainerInspect parse_inspect(const std::string& body) {
    return guard_parse("inspect_container", body, [&] { return parse_inspect_unguarded(body); });
}

static ContainerInspect parse_inspect_unguarded(const std::string& body) {
    const nlohmann::json json = nlohmann::json::parse(body);

    ContainerInspect info;
    info.id = json.value("Id", std::string{});
    info.name = json.value("Name", std::string{});

    if (const auto config = json.find("Config"); config != json.end() && config->is_object()) {
        info.tty = config->value("Tty", false);
    }

    if (const auto state = json.find("State"); state != json.end() && state->is_object()) {
        info.status = state->value("Status", std::string{});
        info.running = state->value("Running", false);
        if (const auto code = state->find("ExitCode");
            code != state->end() && code->is_number_integer()) {
            info.exit_code = code->get<std::int64_t>();
        }
        if (const auto health = state->find("Health");
            health != state->end() && health->is_object()) {
            if (const auto status = health->find("Status");
                status != health->end() && status->is_string()) {
                info.health_status = status->get<std::string>();
            }
        }
    }

    if (const auto net = json.find("NetworkSettings"); net != json.end() && net->is_object()) {
        if (const auto ports = net->find("Ports"); ports != net->end() && ports->is_object()) {
            for (const auto& [port_key, bindings] : ports->items()) {
                std::vector<PortBinding> parsed;
                if (bindings.is_array()) {
                    for (const auto& binding : bindings) {
                        PortBinding pb;
                        pb.host_ip = binding.value("HostIp", std::string{});
                        const std::string host_port = binding.value("HostPort", std::string{});
                        if (!host_port.empty()) {
                            // A malformed/out-of-range HostPort drops the whole
                            // binding: keeping it at host_port=0 would make the
                            // IPv4-preferring selection loops pick a garbage IPv4
                            // binding over a valid IPv6 one and connect to port 0.
                            // (An empty HostPort — a real daemon behavior for
                            // unpublished ports — keeps the binding with 0.)
                            std::uint16_t hp = 0;
                            const auto [end, err] = std::from_chars(
                                host_port.data(), host_port.data() + host_port.size(), hp);
                            if (err != std::errc{} || end != host_port.data() + host_port.size() ||
                                hp == 0) {
                                continue;
                            }
                            pb.host_port = hp;
                        }
                        parsed.push_back(std::move(pb));
                    }
                }
                info.ports.emplace(port_key, std::move(parsed));
            }
        }
    }

    return info;
}

static std::vector<ContainerSummary> parse_container_list_unguarded(const std::string& body);

std::vector<ContainerSummary> parse_container_list(const std::string& body) {
    return guard_parse("list_containers", body,
                       [&] { return parse_container_list_unguarded(body); });
}

static std::vector<ContainerSummary> parse_container_list_unguarded(const std::string& body) {
    const nlohmann::json json = nlohmann::json::parse(body);

    std::vector<ContainerSummary> out;
    if (!json.is_array()) {
        return out;
    }
    for (const auto& entry : json) {
        ContainerSummary summary;
        summary.id = entry.value("Id", std::string{});
        summary.image = entry.value("Image", std::string{});
        summary.state = entry.value("State", std::string{});
        if (const auto names = entry.find("Names"); names != entry.end() && names->is_array()) {
            for (const auto& name : *names) {
                if (name.is_string()) {
                    summary.names.push_back(name.get<std::string>());
                }
            }
        }
        if (const auto labels = entry.find("Labels");
            labels != entry.end() && labels->is_object()) {
            for (const auto& [key, value] : labels->items()) {
                if (value.is_string()) {
                    summary.labels.emplace(key, value.get<std::string>());
                }
            }
        }
        out.push_back(std::move(summary));
    }
    return out;
}

nlohmann::json build_exec_create_body(const std::vector<std::string>& cmd,
                                      const ExecOptions& opts) {
    nlohmann::json body;
    body["Cmd"] = cmd;
    body["AttachStdout"] = true;
    body["AttachStderr"] = true;
    // Only attach stdin when there is input to feed; otherwise a reader inside the
    // container could block waiting on a stream that never closes.
    if (opts.stdin_data) {
        body["AttachStdin"] = true;
    }
    body["Tty"] = opts.tty;
    if (!opts.env.empty()) {
        body["Env"] = opts.env;
    }
    if (opts.working_dir) {
        body["WorkingDir"] = *opts.working_dir;
    }
    if (opts.user) {
        body["User"] = *opts.user;
    }
    if (opts.privileged) {
        body["Privileged"] = true;
    }
    return body;
}

std::string expect_string_field(const std::string& body, const char* field,
                                const std::string& context) {
    try {
        return nlohmann::json::parse(body).at(field).get<std::string>();
    } catch (const nlohmann::json::exception& e) {
        throw DockerError(context + ": unexpected response from Docker (no string '" +
                          std::string(field) + "'): " + e.what() + "; body: " + body_excerpt(body));
    }
}

std::int64_t parse_exec_exit_code(const std::string& body) {
    return guard_parse("exec inspect", body, [&]() -> std::int64_t {
        const nlohmann::json json = nlohmann::json::parse(body);
        if (const auto code = json.find("ExitCode");
            code != json.end() && code->is_number_integer()) {
            return code->get<std::int64_t>();
        }
        return 0;
    });
}

void throw_if_pull_error(const std::string& pull_stream, const std::string& image) {
    std::size_t start = 0;
    while (start < pull_stream.size()) {
        const std::size_t nl = pull_stream.find('\n', start);
        const std::string line =
            pull_stream.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        start = (nl == std::string::npos) ? pull_stream.size() : nl + 1;
        if (line.empty()) {
            continue;
        }
        try {
            const nlohmann::json json = nlohmann::json::parse(line);
            // ANY "error" key is the daemon reporting a failed pull (docker's own
            // jsonmessage errors on any non-nil error, regardless of shape) — dump
            // a non-string payload rather than swallowing the failure.
            if (const auto err = json.find("error"); err != json.end()) {
                throw DockerError("Failed to pull image '" + image + "': " +
                                      (err->is_string() ? err->get<std::string>() : err->dump()),
                                  std::nullopt, image);
            }
        } catch (const nlohmann::json::parse_error&) {
            // Best-effort parse: a non-JSON line (shouldn't happen) is ignored.
        }
    }
}

std::pair<std::string, std::string> split_image(const std::string& image) {
    const std::size_t slash = image.rfind('/');
    const std::size_t colon = image.rfind(':');
    if (colon != std::string::npos && (slash == std::string::npos || colon > slash)) {
        return {image.substr(0, colon), image.substr(colon + 1)};
    }
    return {image, "latest"};
}

std::string build_build_query(const BuildOptions& options,
                              const std::function<std::string(const std::string&)>& encode) {
    std::string query;
    auto append = [&](const std::string& key, const std::string& value) {
        query += query.empty() ? '?' : '&';
        query += key;
        query += '=';
        query += encode(value);
    };
    append("t", options.tag);
    append("dockerfile", options.dockerfile);
    // The legacy builder keeps the FAILED step's intermediate container around
    // "for debugging" (rm=1, the default, only removes them on success). A test
    // library must not leak: forcerm removes them on failure too. The container
    // carries no testcontainers labels, so Ryuk could not reap it either.
    append("forcerm", "1");
    if (options.no_cache) {
        append("nocache", "1");
    }
    if (options.pull) {
        append("pull", "1");
    }
    if (!options.target.empty()) {
        append("target", options.target);
    }
    if (!options.build_args.empty()) {
        nlohmann::json args = nlohmann::json::object();
        for (const auto& [key, value] : options.build_args) {
            args[key] = value;
        }
        append("buildargs", args.dump());
    }
    return query;
}

void throw_if_build_error(const std::string& build_stream, const std::string& tag) {
    std::size_t start = 0;
    while (start < build_stream.size()) {
        const std::size_t nl = build_stream.find('\n', start);
        const std::string line =
            build_stream.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        start = (nl == std::string::npos) ? build_stream.size() : nl + 1;
        if (line.empty()) {
            continue;
        }
        try {
            const nlohmann::json json = nlohmann::json::parse(line);
            // ANY "error" key means the build failed (see throw_if_pull_error).
            if (const auto err = json.find("error"); err != json.end()) {
                throw DockerError("image build failed: " +
                                      (err->is_string() ? err->get<std::string>() : err->dump()),
                                  std::nullopt, tag);
            }
            if (const auto detail = json.find("errorDetail"); detail != json.end()) {
                const auto msg = detail->is_object() ? detail->find("message") : detail->end();
                throw DockerError("image build failed: " + (msg != detail->end() && msg->is_string()
                                                                ? msg->get<std::string>()
                                                                : detail->dump()),
                                  std::nullopt, tag);
            }
        } catch (const nlohmann::json::parse_error&) {
            // Best-effort parse: a non-JSON line (shouldn't happen) is ignored.
        }
    }
}

} // namespace testcontainers::docker
