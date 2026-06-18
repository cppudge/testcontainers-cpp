#include "testcontainers/Container.hpp"

#include <string>
#include <vector>

#include "testcontainers/Error.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"

namespace testcontainers {

std::uint16_t Container::get_host_port(ContainerPort port) const {
    const ContainerInspect info = client_.inspect_container(id_);
    const std::string key = to_string(port); // e.g. "6379/tcp"

    const auto it = info.ports.find(key);
    if (it == info.ports.end() || it->second.empty()) {
        throw DockerError("Container " + id_ + " has no published host port for " + key);
    }
    // Docker may publish different host ports for the IPv4 (0.0.0.0) and IPv6
    // (::) bindings of the same container port. Prefer the IPv4 binding so a
    // connection to a "localhost" that resolves to 127.0.0.1 reaches the right
    // port; fall back to the first binding if only IPv6 is published.
    for (const PortBinding& binding : it->second) {
        if (binding.host_ip.find(':') == std::string::npos) { // IPv4 (or empty host IP)
            return binding.host_port;
        }
    }
    return it->second.front().host_port;
}

ContainerLogs Container::logs() const { return client_.logs(id_); }

ExecResult Container::exec(const std::vector<std::string>& cmd) const {
    return client_.exec(id_, cmd);
}

void Container::stop() { client_.stop_container(id_); }

bool Container::is_running() const { return client_.inspect_container(id_).running; }

void Container::remove() { drop(); }

void Container::drop() noexcept {
    if (dropped_) {
        return;
    }
    dropped_ = true;
    try {
        client_.remove_container(id_, /*force*/ true, /*remove_volumes*/ true);
    } catch (...) {
        // Best-effort: a teardown failure must never propagate (esp. from the
        // destructor).
    }
}

} // namespace testcontainers
