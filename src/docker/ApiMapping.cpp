#include "docker/ApiMapping.hpp"

#include "testcontainers/Error.hpp"

#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

/// Read an object of string values at `key` into a map. Absent / null / wrong
/// type become an empty map (Docker emits `null` for empty Labels / Options).
std::map<std::string, std::string> read_string_map(const nlohmann::json& json, const char* key) {
    std::map<std::string, std::string> out;
    if (const auto it = json.find(key); it != json.end() && it->is_object()) {
        for (const auto& [name, value] : it->items()) {
            out.emplace(name, value.get<std::string>());
        }
    }
    return out;
}

/// Read an array of strings at `key`. Absent / null become an empty vector
/// (Docker emits `null` for an empty Entrypoint / RepoTags / ...).
std::vector<std::string> read_string_array(const nlohmann::json& json, const char* key) {
    std::vector<std::string> out;
    if (const auto it = json.find(key); it != json.end() && it->is_array()) {
        for (const auto& value : *it) {
            out.push_back(value.get<std::string>());
        }
    }
    return out;
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
    if (spec.nano_cpus) {
        host_config["NanoCpus"] = *spec.nano_cpus;
    }
    if (spec.cpuset_cpus) {
        host_config["CpusetCpus"] = *spec.cpuset_cpus;
    }
    if (spec.pids_limit) {
        host_config["PidsLimit"] = *spec.pids_limit;
    }
    if (spec.restart_policy) {
        nlohmann::json policy = nlohmann::json::object();
        policy["Name"] = spec.restart_policy->name;
        policy["MaximumRetryCount"] = spec.restart_policy->maximum_retry_count;
        host_config["RestartPolicy"] = std::move(policy);
    }
    if (!spec.dns_servers.empty()) {
        host_config["Dns"] = spec.dns_servers;
    }
    if (!spec.dns_search.empty()) {
        host_config["DnsSearch"] = spec.dns_search;
    }
    if (!spec.dns_options.empty()) {
        host_config["DnsOptions"] = spec.dns_options;
    }
    if (!spec.sysctls.empty()) {
        nlohmann::json sysctls = nlohmann::json::object();
        for (const auto& [key, value] : spec.sysctls) {
            sysctls[key] = value;
        }
        host_config["Sysctls"] = std::move(sysctls);
    }
    if (!spec.devices.empty()) {
        nlohmann::json devices = nlohmann::json::array();
        for (const Device& d : spec.devices) {
            nlohmann::json entry = nlohmann::json::object();
            entry["PathOnHost"] = d.path_on_host;
            entry["PathInContainer"] = d.path_in_container;
            entry["CgroupPermissions"] = d.cgroup_permissions;
            devices.push_back(std::move(entry));
        }
        host_config["Devices"] = std::move(devices);
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

    // Aliases and a static IP both configure the endpoint on a specific target
    // network; without one there is nothing to attach them to, so they are
    // ignored (no-op) when `network` is unset.
    if (spec.network && (!spec.network_aliases.empty() || spec.static_ipv4)) {
        nlohmann::json endpoint = nlohmann::json::object();
        if (!spec.network_aliases.empty()) {
            endpoint["Aliases"] = spec.network_aliases;
        }
        if (spec.static_ipv4) {
            endpoint["IPAMConfig"]["IPv4Address"] = *spec.static_ipv4;
        }
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

        info.labels = read_string_map(json, "Labels");
        info.options = read_string_map(json, "Options");

        return info;
    });
}

NetworkInspect parse_network_inspect(const std::string& body) {
    return guard_parse("inspect_network", body, [&] {
        const nlohmann::json json = nlohmann::json::parse(body);

        NetworkInspect info;
        info.id = json.value("Id", std::string{});
        info.name = json.value("Name", std::string{});
        info.driver = json.value("Driver", std::string{});
        info.scope = json.value("Scope", std::string{});
        info.internal = json.value("Internal", false);
        info.attachable = json.value("Attachable", false);
        info.enable_ipv6 = json.value("EnableIPv6", false);
        info.options = read_string_map(json, "Options");
        info.labels = read_string_map(json, "Labels");

        if (const auto ipam = json.find("IPAM"); ipam != json.end() && ipam->is_object()) {
            if (const auto config = ipam->find("Config");
                config != ipam->end() && config->is_array()) {
                for (const auto& pool : *config) {
                    if (!pool.is_object()) {
                        continue;
                    }
                    NetworkIpamPool parsed;
                    parsed.subnet = pool.value("Subnet", std::string{});
                    parsed.gateway = pool.value("Gateway", std::string{});
                    info.ipam_pools.push_back(std::move(parsed));
                }
            }
        }

        if (const auto containers = json.find("Containers");
            containers != json.end() && containers->is_object()) {
            for (const auto& [id, endpoint] : containers->items()) {
                NetworkEndpoint parsed;
                if (endpoint.is_object()) {
                    parsed.name = endpoint.value("Name", std::string{});
                    parsed.endpoint_id = endpoint.value("EndpointID", std::string{});
                    parsed.mac_address = endpoint.value("MacAddress", std::string{});
                    parsed.ipv4_address = endpoint.value("IPv4Address", std::string{});
                    parsed.ipv6_address = endpoint.value("IPv6Address", std::string{});
                }
                info.containers.emplace(id, std::move(parsed));
            }
        }

        return info;
    });
}

ImageInspect parse_image_inspect(const std::string& body) {
    return guard_parse("inspect_image", body, [&] {
        const nlohmann::json json = nlohmann::json::parse(body);

        ImageInspect info;
        info.id = json.value("Id", std::string{});
        info.repo_tags = read_string_array(json, "RepoTags");
        info.repo_digests = read_string_array(json, "RepoDigests");
        info.created = json.value("Created", std::string{});
        info.architecture = json.value("Architecture", std::string{});
        info.os = json.value("Os", std::string{});
        info.size = json.value("Size", std::int64_t{0});

        if (const auto config = json.find("Config"); config != json.end() && config->is_object()) {
            info.labels = read_string_map(*config, "Labels");
            info.env = read_string_array(*config, "Env");
            info.cmd = read_string_array(*config, "Cmd");
            info.entrypoint = read_string_array(*config, "Entrypoint");
            info.working_dir = config->value("WorkingDir", std::string{});
            info.user = config->value("User", std::string{});
            if (const auto ports = config->find("ExposedPorts");
                ports != config->end() && ports->is_object()) {
                // An object whose KEYS are the ports ("6379/tcp": {}).
                for (const auto& item : ports->items()) {
                    info.exposed_ports.push_back(item.key());
                }
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

std::string negotiate_api_version(std::string_view daemon_reported) {
    // Parse "major.minor" (digits only, both parts required). Anything else —
    // an empty header, a proxy's HTML, a future exotic scheme — yields nullopt
    // and the caller falls back to unversioned paths.
    const auto parse = [](std::string_view v) -> std::optional<std::pair<unsigned, unsigned>> {
        const std::size_t dot = v.find('.');
        if (dot == std::string_view::npos || dot == 0 || dot + 1 == v.size()) {
            return std::nullopt;
        }
        unsigned major = 0;
        unsigned minor = 0;
        const auto [mp, mec] = std::from_chars(v.data(), v.data() + dot, major);
        const auto [np, nec] = std::from_chars(v.data() + dot + 1, v.data() + v.size(), minor);
        if (mec != std::errc{} || nec != std::errc{} || mp != v.data() + dot ||
            np != v.data() + v.size()) {
            return std::nullopt;
        }
        return std::pair{major, minor};
    };

    const auto daemon = parse(daemon_reported);
    const auto client = parse(kClientApiVersion);
    if (!daemon || !client) {
        return {};
    }
    return *daemon < *client ? std::string(daemon_reported) : std::string(kClientApiVersion);
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

    if (const auto host = json.find("HostConfig"); host != json.end() && host->is_object()) {
        HostConfigInspect& hc = info.host_config;
        hc.memory_bytes = host->value("Memory", std::int64_t{0});
        hc.shm_size_bytes = host->value("ShmSize", std::int64_t{0});
        hc.nano_cpus = host->value("NanoCpus", std::int64_t{0});
        hc.cpuset_cpus = host->value("CpusetCpus", std::string{});
        // Docker reports "no pids limit" as null on newer daemons and 0 on
        // older ones; only a real number lands in the optional (a surfaced 0
        // still means "no limit set", per the struct doc).
        if (const auto pids = host->find("PidsLimit");
            pids != host->end() && pids->is_number_integer()) {
            hc.pids_limit = pids->get<std::int64_t>();
        }
        if (const auto policy = host->find("RestartPolicy");
            policy != host->end() && policy->is_object()) {
            hc.restart_policy.name = policy->value("Name", std::string{});
            hc.restart_policy.maximum_retry_count = policy->value("MaximumRetryCount", 0);
        }
        hc.dns_servers = read_string_array(*host, "Dns");
        hc.dns_search = read_string_array(*host, "DnsSearch");
        hc.dns_options = read_string_array(*host, "DnsOptions");
        hc.sysctls = read_string_map(*host, "Sysctls");
        if (const auto devices = host->find("Devices");
            devices != host->end() && devices->is_array()) {
            for (const auto& entry : *devices) {
                Device device;
                device.path_on_host = entry.value("PathOnHost", std::string{});
                device.path_in_container = entry.value("PathInContainer", std::string{});
                device.cgroup_permissions = entry.value("CgroupPermissions", std::string{});
                hc.devices.push_back(std::move(device));
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
    if (!opts.detach) {
        body["AttachStdout"] = true;
        body["AttachStderr"] = true;
        // Only attach stdin when there is input to feed; otherwise a reader inside
        // the container could block waiting on a stream that never closes. A
        // detached exec attaches nothing at all (the detach+stdin combination is
        // rejected before this is built).
        if (opts.stdin_data) {
            body["AttachStdin"] = true;
        }
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

ExecStatus parse_exec_status(const std::string& body) {
    return guard_parse("exec inspect", body, [&]() -> ExecStatus {
        const nlohmann::json json = nlohmann::json::parse(body);
        ExecStatus status;
        status.running = json.value("Running", false);
        // ExitCode is a moby pointer type: null while the command runs (a
        // value() lookup would throw on present-but-null), an integer after.
        if (const auto code = json.find("ExitCode");
            code != json.end() && code->is_number_integer()) {
            status.exit_code = code->get<std::int64_t>();
        }
        return status;
    });
}

std::int64_t parse_exec_exit_code(const std::string& body) {
    return parse_exec_status(body).exit_code.value_or(0);
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
    if (!options.labels.empty()) {
        nlohmann::json labels = nlohmann::json::object();
        for (const auto& [key, value] : options.labels) {
            labels[key] = value;
        }
        append("labels", labels.dump());
    }
    return query;
}

BuildStreamScanner::BuildStreamScanner(std::string tag, BuildLogConsumer consumer)
    : tag_(std::move(tag)), consumer_(std::move(consumer)) {}

void BuildStreamScanner::feed(std::string_view chunk) {
    pending_.append(chunk);
    std::size_t start = 0;
    for (std::size_t nl = pending_.find('\n', start); nl != std::string::npos;
         nl = pending_.find('\n', start)) {
        scan_line(pending_.substr(start, nl - start));
        start = nl + 1;
    }
    pending_.erase(0, start);
}

void BuildStreamScanner::finish() {
    if (!pending_.empty()) {
        // A well-formed stream ends in '\n'; scan a trailing fragment anyway so
        // a truncated error line is not lost.
        scan_line(pending_);
        pending_.clear();
    }
    if (error_) {
        std::string message = "image build failed: " + *error_;
        if (!tail_.empty()) {
            message += "\n--- build output (tail) ---\n" + tail_;
        }
        throw DockerError(message, std::nullopt, tag_);
    }
}

void BuildStreamScanner::scan_line(const std::string& line) {
    if (line.empty()) {
        return;
    }
    try {
        const nlohmann::json json = nlohmann::json::parse(line);
        if (const auto out = json.find("stream"); out != json.end() && out->is_string()) {
            const std::string& text = out->get_ref<const std::string&>();
            if (consumer_) {
                consumer_(text);
            }
            // Keep only the LAST few KB: with a failing RUN the useful context
            // is the output right before the error, not the whole build log.
            constexpr std::size_t kTailCap = 4096;
            tail_.append(text);
            if (tail_.size() > kTailCap) {
                tail_.erase(0, tail_.size() - kTailCap);
            }
            return;
        }
        if (error_) {
            return; // first error wins; later lines only get drained
        }
        // ANY "error" key means the build failed (see throw_if_pull_error).
        if (const auto err = json.find("error"); err != json.end()) {
            error_ = err->is_string() ? err->get<std::string>() : err->dump();
            return;
        }
        if (const auto detail = json.find("errorDetail"); detail != json.end()) {
            const auto msg = detail->is_object() ? detail->find("message") : detail->end();
            error_ = (msg != detail->end() && msg->is_string()) ? msg->get<std::string>()
                                                                : detail->dump();
        }
    } catch (const nlohmann::json::parse_error&) {
        // Best-effort parse: a non-JSON line (shouldn't happen) is ignored.
    }
}

} // namespace testcontainers::docker
