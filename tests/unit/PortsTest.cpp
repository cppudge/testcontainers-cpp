#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "docker/Ports.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"

// Tests in this file:
//   Ports.SelectAnyPrefersIpv4 - Any returns the IPv4 binding when both IPv4 and IPv6 are present.
//   Ports.SelectAnyFallsBackToIpv6 - Any returns the lone IPv6 binding when no IPv4 binding exists.
//   Ports.SelectAnyMissingKey - Any returns nullopt for a key with no binding.
//   Ports.SelectIpv4Only - Ipv4 returns only the IPv4 binding (nullopt when only IPv6 present); empty host_ip counts as IPv4.
//   Ports.SelectIpv6Only - Ipv6 returns only the IPv6 binding (nullopt when only IPv4 present).
//   Ports.LowestPublishedPicksLowestKey - lowest_published_host_port picks the lowest-numbered key's binding.
//   Ports.LowestPublishedEmpty - lowest_published_host_port returns nullopt for an empty map.

using namespace testcontainers;
using testcontainers::docker::HostPortFamily;
using testcontainers::docker::lowest_published_host_port;
using testcontainers::docker::select_host_port;

namespace {

using PortsMap = std::map<std::string, std::vector<PortBinding>>;

PortBinding ipv4(std::uint16_t hp) { return PortBinding{"0.0.0.0", hp}; }
PortBinding ipv6(std::uint16_t hp) { return PortBinding{"::", hp}; }

} // namespace

TEST(Ports, SelectAnyPrefersIpv4) {
    // Docker often publishes a separate host port for the IPv6 binding; Any must
    // prefer the IPv4 one regardless of binding order.
    const PortsMap ports{{"6379/tcp", {ipv6(40001), ipv4(40000)}}};
    EXPECT_EQ(select_host_port(ports, "6379/tcp", HostPortFamily::Any), 40000);
}

TEST(Ports, SelectAnyFallsBackToIpv6) {
    const PortsMap ports{{"6379/tcp", {ipv6(50000)}}};
    EXPECT_EQ(select_host_port(ports, "6379/tcp", HostPortFamily::Any), 50000);
}

TEST(Ports, SelectAnyMissingKey) {
    const PortsMap ports{{"6379/tcp", {ipv4(40000)}}};
    EXPECT_FALSE(select_host_port(ports, "9999/tcp", HostPortFamily::Any).has_value());
    // A key present but with no bindings is also "unmapped".
    const PortsMap empty_binding{{"6379/tcp", {}}};
    EXPECT_FALSE(select_host_port(empty_binding, "6379/tcp", HostPortFamily::Any).has_value());
}

TEST(Ports, SelectIpv4Only) {
    const PortsMap both{{"6379/tcp", {ipv6(40001), ipv4(40000)}}};
    EXPECT_EQ(select_host_port(both, "6379/tcp", HostPortFamily::Ipv4), 40000);

    // Only IPv6 present -> no IPv4 binding.
    const PortsMap ipv6_only{{"6379/tcp", {ipv6(50000)}}};
    EXPECT_FALSE(select_host_port(ipv6_only, "6379/tcp", HostPortFamily::Ipv4).has_value());

    // An empty host_ip is treated as IPv4.
    const PortsMap empty_ip{{"6379/tcp", {PortBinding{"", 32768}}}};
    EXPECT_EQ(select_host_port(empty_ip, "6379/tcp", HostPortFamily::Ipv4), 32768);
}

TEST(Ports, SelectIpv6Only) {
    const PortsMap both{{"6379/tcp", {ipv4(40000), ipv6(40001)}}};
    EXPECT_EQ(select_host_port(both, "6379/tcp", HostPortFamily::Ipv6), 40001);

    // Only IPv4 present -> no IPv6 binding.
    const PortsMap ipv4_only{{"6379/tcp", {ipv4(40000)}}};
    EXPECT_FALSE(select_host_port(ipv4_only, "6379/tcp", HostPortFamily::Ipv6).has_value());
}

TEST(Ports, LowestPublishedPicksLowestKey) {
    // 8080 is the lowest-numbered key, so its binding's host port wins (not the
    // first in std::map order, which is the same here, but the intent is numeric).
    const PortsMap ports{
        {"9090/tcp", {ipv4(49090)}},
        {"8080/tcp", {ipv4(48080)}},
    };
    EXPECT_EQ(lowest_published_host_port(ports, HostPortFamily::Any), 48080);

    // Keys are compared numerically, not lexicographically: "9/tcp" < "10/tcp".
    const PortsMap numeric{
        {"10/tcp", {ipv4(40010)}},
        {"9/tcp", {ipv4(40009)}},
    };
    EXPECT_EQ(lowest_published_host_port(numeric, HostPortFamily::Any), 40009);
}

TEST(Ports, LowestPublishedEmpty) {
    const PortsMap empty;
    EXPECT_FALSE(lowest_published_host_port(empty, HostPortFamily::Any).has_value());

    // A map whose only entry has no binding under the requested family yields
    // nullopt too.
    const PortsMap ipv6_only{{"8080/tcp", {ipv6(48080)}}};
    EXPECT_FALSE(lowest_published_host_port(ipv6_only, HostPortFamily::Ipv4).has_value());
}
