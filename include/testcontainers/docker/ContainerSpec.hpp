#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/Healthcheck.hpp"
#include "testcontainers/Mount.hpp"

namespace testcontainers {

/// Minimal request for `POST /containers/create`.
///
/// This is a deliberately small, flat precursor to the full `ContainerRequest`
/// model; it carries just enough to create and run a container.
struct CreateContainerSpec {
    std::string image;                                       ///< "alpine:3.20"
    std::vector<std::string> cmd;                            ///< command / args
    std::vector<std::string> entrypoint;                     ///< Entrypoint override
    std::vector<std::string> env;                            ///< "KEY=VALUE" entries
    std::vector<std::string> exposed_ports;                  ///< "6379/tcp"
    std::vector<std::pair<std::string, std::string>> labels; ///< container labels
    std::optional<std::string> name;                         ///< container name (?name=)
    std::optional<std::string> platform;                     ///< "<os>/<arch>" (?platform=)
    std::optional<std::string> network;                      ///< HostConfig.NetworkMode
    std::optional<std::string> working_dir;                  ///< WorkingDir
    std::optional<std::string> user;                         ///< User
    bool publish_all_ports = false;                          ///< HostConfig.PublishAllPorts
    bool privileged = false;                                 ///< HostConfig.Privileged
    bool auto_remove = false;                                ///< HostConfig.AutoRemove
    std::vector<Mount> mounts;                               ///< HostConfig.Mounts
    std::optional<Healthcheck> healthcheck;                  ///< Healthcheck (create-body)
};

/// A single published port binding from a container inspect.
struct PortBinding {
    std::string host_ip;          ///< "0.0.0.0" / "::"
    std::uint16_t host_port = 0;  ///< host-side port
};

/// A subset of one entry from `GET /containers/json` (the list endpoint).
struct ContainerSummary {
    std::string id;
    std::vector<std::string> names;                 ///< "/my-svc" style names
    std::string image;
    std::string state;                              ///< "running" / "exited" / ...
    std::map<std::string, std::string> labels;      ///< container labels
};

/// The subset of `GET /containers/{id}/json` we currently care about.
struct ContainerInspect {
    std::string id;
    std::string name;
    std::string status;                       ///< State.Status ("running", "exited", …)
    bool running = false;                      ///< State.Running
    std::optional<std::int64_t> exit_code;     ///< State.ExitCode (when not running)
    /// State.Health.Status ("starting"/"healthy"/"unhealthy"); absent when the
    /// container has no healthcheck configured.
    std::optional<std::string> health_status;
    /// "6379/tcp" -> published host bindings (from NetworkSettings.Ports).
    std::map<std::string, std::vector<PortBinding>> ports;
};

} // namespace testcontainers
