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
    if (!spec.env.empty()) {
        body["Env"] = spec.env;
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

    nlohmann::json host_config = nlohmann::json::object();
    if (spec.publish_all_ports) {
        host_config["PublishAllPorts"] = true;
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
