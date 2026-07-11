#include "HostAddress.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "Config.hpp"
#include "FileRead.hpp"

namespace testcontainers {
namespace detail {

namespace {

/// True when the test process itself runs inside a Linux container (the
/// engine drops /.dockerenv into every container it starts). On Windows the
/// path resolves against the current drive's root, where no such file exists
/// in practice.
bool running_inside_container() {
    std::error_code ec;
    return std::filesystem::exists("/.dockerenv", ec);
}

/// Whitespace-split `line` into its columns.
std::vector<std::string_view> split_columns(std::string_view line) {
    std::vector<std::string_view> columns;
    std::size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
            ++pos;
        }
        const std::size_t start = pos;
        while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') {
            ++pos;
        }
        if (pos > start) {
            columns.push_back(line.substr(start, pos - start));
        }
    }
    return columns;
}

} // namespace

std::optional<std::string> parse_default_gateway(std::string_view route_table) {
    std::size_t line_start = 0;
    bool header = true;
    while (line_start <= route_table.size()) {
        const std::size_t line_end = route_table.find('\n', line_start);
        const std::string_view line = route_table.substr(
            line_start,
            line_end == std::string_view::npos ? std::string_view::npos : line_end - line_start);
        line_start = line_end == std::string_view::npos ? route_table.size() + 1 : line_end + 1;

        if (header) { // the "Iface Destination Gateway ..." title row
            header = false;
            continue;
        }
        // Columns: Iface Destination Gateway Flags RefCnt Use Metric Mask ...
        const std::vector<std::string_view> columns = split_columns(line);
        if (columns.size() < 8 || columns[1] != "00000000" || columns[7] != "00000000") {
            continue; // not the default route
        }
        const std::string_view gateway_hex = columns[2];
        std::uint32_t gateway = 0;
        const auto [end, err] = std::from_chars(
            gateway_hex.data(), gateway_hex.data() + gateway_hex.size(), gateway, 16);
        if (err != std::errc{} || end != gateway_hex.data() + gateway_hex.size()) {
            continue; // malformed hex — keep scanning
        }
        if (gateway == 0) {
            continue; // an on-link default route has no gateway to hand out
        }
        // The kernel prints the address little-endian: byte 0 is the first octet.
        return std::to_string(gateway & 0xFF) + "." + std::to_string((gateway >> 8) & 0xFF) + "." +
               std::to_string((gateway >> 16) & 0xFF) + "." +
               std::to_string((gateway >> 24) & 0xFF);
    }
    return std::nullopt;
}

std::string resolved_host_address(const DockerHost& host) {
    if (const std::optional<std::string> override_address =
            config_value("TESTCONTAINERS_HOST_OVERRIDE", "host.override")) {
        return *override_address;
    }

    switch (host.scheme()) {
    case DockerScheme::Tcp:
    case DockerScheme::Https:
        return host.http_host();
    case DockerScheme::Unix:
    case DockerScheme::NamedPipe:
        break;
    }

    // A local socket/pipe normally means published ports listen on localhost.
    // Inside a Linux container (DinD with a socket mount) localhost is the
    // container itself — the default gateway is the bridge address where the
    // daemon's published ports actually answer.
    if (running_inside_container()) {
        if (const std::optional<std::string> gateway =
                parse_default_gateway(read_file("/proc/net/route"))) {
            return *gateway;
        }
    }
    return "localhost";
}

} // namespace detail
} // namespace testcontainers
