#pragma once

#include <cstdint>
#include <string>

namespace testcontainers {

/// Transport protocol of a container port.
enum class Proto { Tcp, Udp, Sctp };

/// A port exposed by a container, with its protocol.
///
/// A plain, copyable, equality-comparable value type (no opaque handles, no
/// move-only semantics) — it slots straight into a `std::vector`.
struct ContainerPort {
    std::uint16_t port = 0;
    Proto proto = Proto::Tcp;
};

inline bool operator==(const ContainerPort& a, const ContainerPort& b) {
    return a.port == b.port && a.proto == b.proto;
}
inline bool operator!=(const ContainerPort& a, const ContainerPort& b) { return !(a == b); }

/// Build a TCP container port (e.g. `tcp(6379)`).
inline ContainerPort tcp(std::uint16_t port) { return {port, Proto::Tcp}; }

/// Build a UDP container port.
inline ContainerPort udp(std::uint16_t port) { return {port, Proto::Udp}; }

/// Build an SCTP container port.
inline ContainerPort sctp(std::uint16_t port) { return {port, Proto::Sctp}; }

/// Format a container port the way Docker names it, e.g. "6379/tcp".
inline std::string to_string(const ContainerPort& p) {
    const char* proto = "tcp";
    switch (p.proto) {
    case Proto::Tcp:
        proto = "tcp";
        break;
    case Proto::Udp:
        proto = "udp";
        break;
    case Proto::Sctp:
        proto = "sctp";
        break;
    }
    return std::to_string(p.port) + "/" + proto;
}

} // namespace testcontainers
