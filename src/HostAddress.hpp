#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "testcontainers/docker/DockerHost.hpp"

namespace testcontainers {
namespace detail {

/// The address a client running in THIS process should use to reach ports
/// published on the daemon's host — what `Container::host()`, the HTTP/port
/// wait probes, the compose service host, and the Ryuk registration all hand
/// out:
///
///   1. `TESTCONTAINERS_HOST_OVERRIDE` / `host.override` when set (any
///      scheme) — the operator's word for "published ports are reachable
///      here" (Docker-in-Docker with a socket mount, Testcontainers Cloud).
///   2. tcp:// / https:// daemons: the daemon hostname.
///   3. A local unix socket / named pipe: "localhost" — unless the test
///      process itself runs inside a Linux container (`/.dockerenv` exists),
///      where localhost is the container, not the daemon's host: the default
///      gateway (the docker bridge address, where published ports listen) is
///      returned instead.
std::string resolved_host_address(const DockerHost& host);

/// Parse the default route's gateway out of a /proc/net/route table: the
/// first data row whose Destination and Mask are both 00000000 and whose
/// Gateway is non-zero; the kernel prints these columns as little-endian
/// hex, so "010011AC" reads back as "172.17.0.1". Returns nullopt when no
/// such row exists (or the table is malformed). Pure — exposed for unit
/// testing.
std::optional<std::string> parse_default_gateway(std::string_view route_table);

} // namespace detail
} // namespace testcontainers
