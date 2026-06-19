#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "testcontainers/docker/ContainerSpec.hpp" // PortBinding

namespace testcontainers::docker {

/// Which host-IP family to select a published binding for.
enum class HostPortFamily {
    Any,  ///< IPv4 binding preferred, else the first binding
    Ipv4, ///< only an IPv4 binding (host_ip without ':', or empty)
    Ipv6, ///< only an IPv6 binding (host_ip contains ':')
};

namespace detail {

/// True when `host_ip` denotes an IPv6 address (contains a ':'). An empty
/// host_ip is treated as IPv4.
inline bool is_ipv6_host_ip(const std::string& host_ip) {
    return host_ip.find(':') != std::string::npos;
}

} // namespace detail

/// The published host port for `key` ("6379/tcp") under `family`, or nullopt if
/// no matching binding exists. (Pure; operates on an already-parsed ports map.)
inline std::optional<std::uint16_t> select_host_port(
    const std::map<std::string, std::vector<PortBinding>>& ports,
    const std::string& key, HostPortFamily family) {
    const auto it = ports.find(key);
    if (it == ports.end() || it->second.empty()) {
        return std::nullopt;
    }
    switch (family) {
    case HostPortFamily::Any:
        // Docker may publish different host ports for the IPv4 (0.0.0.0) and IPv6
        // (::) bindings of the same container port. Prefer the IPv4 binding so a
        // connection to a "localhost" that resolves to 127.0.0.1 reaches the
        // right port; fall back to the first binding if only IPv6 is published.
        for (const PortBinding& binding : it->second) {
            if (!detail::is_ipv6_host_ip(binding.host_ip)) {
                return binding.host_port;
            }
        }
        return it->second.front().host_port;
    case HostPortFamily::Ipv4:
        for (const PortBinding& binding : it->second) {
            if (!detail::is_ipv6_host_ip(binding.host_ip)) {
                return binding.host_port;
            }
        }
        return std::nullopt;
    case HostPortFamily::Ipv6:
        for (const PortBinding& binding : it->second) {
            if (detail::is_ipv6_host_ip(binding.host_ip)) {
                return binding.host_port;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

/// The IPv4-preferred (family) host port of the lowest-numbered published
/// container port, or nullopt if nothing is published. Used for first_mapped_port
/// when the exposed-port order is unknown. Parses the numeric port out of each
/// "NNN/proto" key to find the lowest.
inline std::optional<std::uint16_t> lowest_published_host_port(
    const std::map<std::string, std::vector<PortBinding>>& ports, HostPortFamily family) {
    std::optional<std::uint16_t> best;
    std::optional<long> best_container_port;
    for (const auto& [key, bindings] : ports) {
        if (bindings.empty()) {
            continue;
        }
        // Parse the integer prefix of the key (the part before '/').
        std::size_t slash = key.find('/');
        const std::string num = key.substr(0, slash);
        if (num.empty()) {
            continue;
        }
        long container_port = 0;
        try {
            std::size_t consumed = 0;
            container_port = std::stol(num, &consumed);
            if (consumed != num.size()) {
                continue; // not a clean integer prefix
            }
        } catch (...) {
            continue;
        }
        // Only consider keys that actually have a binding under the requested
        // family.
        const std::optional<std::uint16_t> host_port = select_host_port(ports, key, family);
        if (!host_port) {
            continue;
        }
        if (!best_container_port || container_port < *best_container_port) {
            best_container_port = container_port;
            best = host_port;
        }
    }
    return best;
}

} // namespace testcontainers::docker
