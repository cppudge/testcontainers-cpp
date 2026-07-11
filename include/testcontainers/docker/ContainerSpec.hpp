#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/Device.hpp"
#include "testcontainers/Healthcheck.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/RestartPolicy.hpp"
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
    std::vector<std::string> network_aliases; ///< DNS aliases on `network` (NetworkingConfig)
    /// Fixed IPv4 address on `network` (NetworkingConfig ... IPAMConfig.IPv4Address).
    std::optional<std::string> static_ipv4;
    std::optional<std::string> working_dir; ///< WorkingDir
    std::optional<std::string> user;        ///< User
    bool tty = false;               ///< Tty (allocate a pseudo-TTY; raw/unframed log stream)
    bool publish_all_ports = false; ///< HostConfig.PublishAllPorts
    bool privileged = false;        ///< HostConfig.Privileged
    bool auto_remove = false;       ///< HostConfig.AutoRemove
    std::vector<Mount> mounts;      ///< HostConfig.Mounts
    std::optional<Healthcheck> healthcheck;      ///< Healthcheck (create-body)
    std::optional<std::int64_t> memory_bytes;    ///< HostConfig.Memory (hard limit, bytes)
    std::optional<std::int64_t> shm_size_bytes;  ///< HostConfig.ShmSize (bytes)
    std::optional<std::int64_t> nano_cpus;       ///< HostConfig.NanoCpus (1e9 = one CPU)
    std::optional<std::string> cpuset_cpus;      ///< HostConfig.CpusetCpus ("0-2,7")
    std::optional<std::int64_t> pids_limit;      ///< HostConfig.PidsLimit (-1 = unlimited)
    std::vector<Ulimit> ulimits;                 ///< HostConfig.Ulimits
    std::vector<std::string> cap_add;            ///< HostConfig.CapAdd
    std::vector<std::string> cap_drop;           ///< HostConfig.CapDrop
    std::vector<std::string> extra_hosts;        ///< HostConfig.ExtraHosts ("host:ip")
    std::optional<RestartPolicy> restart_policy; ///< HostConfig.RestartPolicy
    std::vector<std::string> dns_servers;        ///< HostConfig.Dns
    std::vector<std::string> dns_search;         ///< HostConfig.DnsSearch (search domains)
    std::vector<std::string> dns_options;        ///< HostConfig.DnsOptions (resolver options)
    std::vector<std::pair<std::string, std::string>> sysctls; ///< HostConfig.Sysctls
    std::vector<Device> devices;                              ///< HostConfig.Devices
    /// HostConfig.Isolation ("process"/"hyperv"; Windows daemons).
    std::optional<std::string> isolation;
    /// Raw JSON object deep-merged into the create body (escape hatch);
    /// empty = none.
    std::string create_body_patch;
};

/// One IPAM address pool of a network — an `IPAM.Config` entry, both as sent
/// by `POST /networks/create` (`NetworkCreateSpec::ipam_pools`) and as read
/// back by `GET /networks/{id}` (`NetworkInspect::ipam_pools`). An empty
/// field is absent: omitted from the create body, "" when the daemon returns
/// nothing.
struct NetworkIpamPool {
    std::string subnet;   ///< CIDR, e.g. "172.31.250.0/24" (create: required by the daemon)
    std::string gateway;  ///< gateway address on the subnet
    std::string ip_range; ///< IPRange: a sub-CIDR to allocate container addresses from
    /// AuxiliaryAddresses (name -> IP): addresses on the subnet the IPAM driver
    /// must not hand out to containers (e.g. an external router). Create emits
    /// them in order (last wins on a duplicate name); inspect returns them
    /// sorted by name.
    std::vector<std::pair<std::string, std::string>> aux_addresses;
};

/// Options for `POST /networks/create` (richer than just name+labels).
struct NetworkCreateSpec {
    std::string name;
    std::optional<std::string> driver;  ///< e.g. "bridge" (default when unset)
    bool internal = false;              ///< no external connectivity
    bool attachable = false;            ///< standalone containers can attach
    bool enable_ipv6 = false;           ///< EnableIPv6
    std::optional<std::string> subnet;  ///< shorthand: a leading IPAM pool's Subnet
    std::optional<std::string> gateway; ///< shorthand: the leading pool's Gateway
    /// IPAM address pools, emitted after the subnet/gateway shorthand pool
    /// when that is set (`IPAM.Config` keeps this order).
    std::vector<NetworkIpamPool> ipam_pools;
    std::vector<std::pair<std::string, std::string>> options; ///< driver Options
    std::vector<std::pair<std::string, std::string>> labels;  ///< network Labels
};

/// Options for `POST /volumes/create`.
struct VolumeCreateSpec {
    std::string name;                  ///< volume name (we always set it)
    std::optional<std::string> driver; ///< Driver (default "local" when unset)
    std::vector<std::pair<std::string, std::string>> driver_opts; ///< DriverOpts
    std::vector<std::pair<std::string, std::string>> labels;      ///< Labels
};

/// The subset of `GET /volumes/{name}` we care about.
struct VolumeInspect {
    std::string name;                           ///< Name
    std::string driver;                         ///< Driver
    std::string mountpoint;                     ///< Mountpoint (host path on the daemon)
    std::string scope;                          ///< Scope ("local" / "global")
    std::map<std::string, std::string> labels;  ///< Labels (null -> empty)
    std::map<std::string, std::string> options; ///< Options (null -> empty)
};

/// The daemon's report from `POST /volumes/prune`.
struct VolumePruneResult {
    std::vector<std::string> deleted; ///< VolumesDeleted (null -> empty)
    std::int64_t space_reclaimed = 0; ///< SpaceReclaimed, bytes
};

/// One attached container's endpoint on a network (a `Containers` map entry in
/// `GET /networks/{id}`).
struct NetworkEndpoint {
    std::string name;         ///< container name
    std::string endpoint_id;  ///< EndpointID
    std::string mac_address;  ///< MacAddress
    std::string ipv4_address; ///< CIDR form, e.g. "172.18.0.2/16"; "" when absent
    std::string ipv6_address; ///< CIDR form; "" when absent
};

/// The subset of `GET /networks/{id}` we currently care about.
struct NetworkInspect {
    std::string id;
    std::string name;
    std::string driver;    ///< "bridge" / "nat" / ...
    std::string scope;     ///< "local" / "swarm"
    bool internal = false; ///< Internal (no external connectivity)
    bool attachable = false;
    bool enable_ipv6 = false;
    std::vector<NetworkIpamPool> ipam_pools;    ///< IPAM.Config (empty when none)
    std::map<std::string, std::string> options; ///< driver Options (null -> empty)
    std::map<std::string, std::string> labels;  ///< Labels (null -> empty)
    /// Container id -> endpoint, one entry per currently attached container
    /// (null / absent -> empty).
    std::map<std::string, NetworkEndpoint> containers;
};

/// The subset of `GET /images/{reference}/json` we currently care about.
struct ImageInspect {
    std::string id;                            ///< "sha256:..."
    std::vector<std::string> repo_tags;        ///< "redis:7.2"-style references (may be empty)
    std::vector<std::string> repo_digests;     ///< "redis@sha256:..." entries (may be empty)
    std::string created;                       ///< RFC 3339 timestamp, verbatim
    std::string architecture;                  ///< "amd64" / "arm64" / ...
    std::string os;                            ///< "linux" / "windows"
    std::int64_t size = 0;                     ///< image size in bytes
    std::map<std::string, std::string> labels; ///< Config.Labels (null -> empty)
    std::vector<std::string> env;              ///< Config.Env ("KEY=VALUE" entries)
    std::vector<std::string> cmd;              ///< Config.Cmd (null -> empty)
    std::vector<std::string> entrypoint;       ///< Config.Entrypoint (null -> empty)
    std::vector<std::string> exposed_ports;    ///< Config.ExposedPorts keys ("6379/tcp")
    std::string working_dir;                   ///< Config.WorkingDir
    std::string user;                          ///< Config.User
};

/// A single published port binding from a container inspect.
struct PortBinding {
    std::string host_ip;         ///< "0.0.0.0" / "::"
    std::uint16_t host_port = 0; ///< host-side port
};

/// A subset of one entry from `GET /containers/json` (the list endpoint).
struct ContainerSummary {
    std::string id;
    std::vector<std::string> names; ///< "/my-svc" style names
    std::string image;
    std::string state;                         ///< "running" / "exited" / ...
    std::map<std::string, std::string> labels; ///< container labels
};

/// The `HostConfig` subset echoed back by `GET /containers/{id}/json` — the
/// typed create-side knobs, readable back so tests can assert a limit landed.
/// Values are the daemon's echo verbatim: an unset knob reads as Docker's
/// zero state (0 / "" / empty; note ShmSize echoes the daemon's 64 MiB
/// default and RestartPolicy.Name echoes "no" on modern daemons).
struct HostConfigInspect {
    std::int64_t memory_bytes = 0;   ///< Memory; 0 = unlimited
    std::int64_t shm_size_bytes = 0; ///< ShmSize; the daemon's 64 MiB default when unset
    std::int64_t nano_cpus = 0;      ///< NanoCpus; 0 = no CPU limit
    std::string cpuset_cpus;         ///< CpusetCpus; "" = not pinned
    /// PidsLimit; absent when the daemon reports null (no limit; some daemon
    /// versions report 0 instead — both mean "no limit set").
    std::optional<std::int64_t> pids_limit;
    RestartPolicy restart_policy;               ///< RestartPolicy (Name "" or "no" = none)
    std::vector<std::string> dns_servers;       ///< Dns (null -> empty)
    std::vector<std::string> dns_search;        ///< DnsSearch (null -> empty)
    std::vector<std::string> dns_options;       ///< DnsOptions (null -> empty)
    std::map<std::string, std::string> sysctls; ///< Sysctls (null -> empty)
    std::vector<Device> devices;                ///< Devices (null -> empty)
};

/// The subset of `GET /containers/{id}/json` we currently care about.
struct ContainerInspect {
    std::string id;
    std::string name;
    std::string status;                    ///< State.Status ("running", "exited", …)
    bool running = false;                  ///< State.Running
    bool tty = false;                      ///< Config.Tty (container created with a pseudo-TTY)
    std::optional<std::int64_t> exit_code; ///< State.ExitCode (when not running)
    /// State.Health.Status ("starting"/"healthy"/"unhealthy"); absent when the
    /// container has no healthcheck configured.
    std::optional<std::string> health_status;
    /// "6379/tcp" -> published host bindings (from NetworkSettings.Ports).
    std::map<std::string, std::vector<PortBinding>> ports;
    /// The typed HostConfig knobs, echoed back (zero state when absent).
    HostConfigInspect host_config;
};

} // namespace testcontainers
