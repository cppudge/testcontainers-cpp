#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/Healthcheck.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/Ulimit.hpp"

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
    std::vector<std::string> network_aliases;                ///< DNS aliases on `network` (NetworkingConfig)
    std::optional<std::string> working_dir;                  ///< WorkingDir
    std::optional<std::string> user;                         ///< User
    bool tty = false;                                        ///< Tty (allocate a pseudo-TTY; raw/unframed log stream)
    bool publish_all_ports = false;                          ///< HostConfig.PublishAllPorts
    bool privileged = false;                                 ///< HostConfig.Privileged
    bool auto_remove = false;                                ///< HostConfig.AutoRemove
    std::vector<Mount> mounts;                               ///< HostConfig.Mounts
    std::optional<Healthcheck> healthcheck;                  ///< Healthcheck (create-body)
    std::optional<std::int64_t> memory_bytes;                ///< HostConfig.Memory (hard limit, bytes)
    std::optional<std::int64_t> shm_size_bytes;              ///< HostConfig.ShmSize (bytes)
    std::vector<Ulimit> ulimits;                             ///< HostConfig.Ulimits
    std::vector<std::string> cap_add;                        ///< HostConfig.CapAdd
    std::vector<std::string> cap_drop;                       ///< HostConfig.CapDrop
    std::vector<std::string> extra_hosts;                    ///< HostConfig.ExtraHosts ("host:ip")
    std::string create_body_patch;                           ///< raw JSON object deep-merged into the create body (escape hatch); empty = none
};

/// Options for `POST /networks/create` (richer than just name+labels).
struct NetworkCreateSpec {
    std::string name;
    std::optional<std::string> driver;     ///< e.g. "bridge" (default when unset)
    bool internal = false;                 ///< no external connectivity
    bool attachable = false;               ///< standalone containers can attach
    bool enable_ipv6 = false;              ///< EnableIPv6
    std::optional<std::string> subnet;     ///< IPAM.Config[0].Subnet, e.g. "172.31.250.0/24"
    std::optional<std::string> gateway;    ///< IPAM.Config[0].Gateway
    std::vector<std::pair<std::string, std::string>> options; ///< driver Options
    std::vector<std::pair<std::string, std::string>> labels;  ///< network Labels
};

/// Options for `POST /volumes/create`.
struct VolumeCreateSpec {
    std::string name;                                            ///< volume name (we always set it)
    std::optional<std::string> driver;                           ///< Driver (default "local" when unset)
    std::vector<std::pair<std::string, std::string>> driver_opts; ///< DriverOpts
    std::vector<std::pair<std::string, std::string>> labels;     ///< Labels
};

/// The subset of `GET /volumes/{name}` we care about.
struct VolumeInspect {
    std::string name;                          ///< Name
    std::string driver;                        ///< Driver
    std::string mountpoint;                    ///< Mountpoint (host path on the daemon)
    std::string scope;                         ///< Scope ("local" / "global")
    std::map<std::string, std::string> labels; ///< Labels (null -> empty)
    std::map<std::string, std::string> options; ///< Options (null -> empty)
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
    bool tty = false;                          ///< Config.Tty (container created with a pseudo-TTY)
    std::optional<std::int64_t> exit_code;     ///< State.ExitCode (when not running)
    /// State.Health.Status ("starting"/"healthy"/"unhealthy"); absent when the
    /// container has no healthcheck configured.
    std::optional<std::string> health_status;
    /// "6379/tcp" -> published host bindings (from NetworkSettings.Ports).
    std::map<std::string, std::vector<PortBinding>> ports;
};

} // namespace testcontainers
