#pragma once

#include <string>

namespace testcontainers {

/// A host device node mapped into the container (a `HostConfig.Devices`
/// entry) — `docker run --device` parity. Linux daemons only. The host path
/// must exist on the machine the DAEMON runs on: with Docker Desktop that is
/// the Linux VM, not the machine the tests run on.
struct Device {
    std::string path_on_host;               ///< e.g. "/dev/fuse"
    std::string path_in_container;          ///< where the node appears inside
    std::string cgroup_permissions = "rwm"; ///< any subset of r(ead) w(rite) m(knod)
};

} // namespace testcontainers
