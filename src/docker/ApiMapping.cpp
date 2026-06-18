#include "docker/ApiMapping.hpp"

#include "testcontainers/Error.hpp"

#include <cstdint>

namespace testcontainers::docker {

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

    return body;
}

ContainerInspect parse_inspect(const std::string& body) {
    const nlohmann::json json = nlohmann::json::parse(body);

    ContainerInspect info;
    info.id = json.value("Id", std::string{});
    info.name = json.value("Name", std::string{});

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
                            pb.host_port = static_cast<std::uint16_t>(std::stoi(host_port));
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

nlohmann::json build_exec_create_body(const std::vector<std::string>& cmd) {
    nlohmann::json body;
    body["Cmd"] = cmd;
    body["AttachStdout"] = true;
    body["AttachStderr"] = true;
    return body;
}

std::int64_t parse_exec_exit_code(const std::string& body) {
    const nlohmann::json json = nlohmann::json::parse(body);
    if (const auto code = json.find("ExitCode");
        code != json.end() && code->is_number_integer()) {
        return code->get<std::int64_t>();
    }
    return 0;
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
            if (json.contains("error")) {
                throw DockerError("Failed to pull image '" + image +
                                  "': " + json["error"].get<std::string>());
            }
        } catch (const nlohmann::json::parse_error&) {
            // Non-JSON line (shouldn't happen) — ignore.
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

} // namespace testcontainers::docker
