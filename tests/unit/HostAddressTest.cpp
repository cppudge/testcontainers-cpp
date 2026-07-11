#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "HostAddress.hpp"
#include "TempHome.hpp"
#include "TestEnv.hpp"
#include "testcontainers/docker/DockerHost.hpp"

// Tests in this file (detail::resolved_host_address — the Container::host() /
// probe-address rule — and the pure /proc/net/route parser behind its
// in-container branch):
//   HostAddress.ParseDefaultGatewayFindsBridge - the default route's little-endian hex Gateway reads back as the dotted bridge address.
//   HostAddress.ParseDefaultGatewayNoDefaultRoute - an empty/header-only/on-link-only table yields nullopt.
//   HostAddress.ParseDefaultGatewaySkipsUnusableRows - on-link (gateway 0), malformed-hex, and short rows are skipped; a later valid default still lands.
//   HostAddress.ParseDefaultGatewayFirstMatchWins - with two default routes the first one is returned.
//   HostOverrideFile.EnvOverrideWinsOnAnyScheme - a set TESTCONTAINERS_HOST_OVERRIDE is returned verbatim for tcp and unix daemons alike.
//   HostOverrideFile.PropertiesOverrideApplies - with no env override, host.override from ~/.testcontainers.properties decides.
//   HostOverrideFile.TcpDaemonUsesHostname - tcp:// and https:// daemons resolve to their hostname when nothing overrides.
//   HostOverrideFile.LocalSocketIsLocalhost - a unix socket / named pipe resolves to "localhost" (this test process runs outside any container).

namespace {

using tctest::ScopedEnv;
using testcontainers::DockerHost;
using testcontainers::detail::parse_default_gateway;
using testcontainers::detail::resolved_host_address;

/// A realistic /proc/net/route: the header row, the default route through
/// 172.17.0.1 (010011AC little-endian), and the on-link subnet route.
constexpr const char* kRouteTable =
    "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
    "eth0\t00000000\t010011AC\t0003\t0\t0\t0\t00000000\t0\t0\t0\n"
    "eth0\t000011AC\t00000000\t0001\t0\t0\t0\t0000FFFF\t0\t0\t0\n";

} // namespace

TEST(HostAddress, ParseDefaultGatewayFindsBridge) {
    const auto gateway = parse_default_gateway(kRouteTable);
    ASSERT_TRUE(gateway.has_value());
    EXPECT_EQ(*gateway, "172.17.0.1");
}

TEST(HostAddress, ParseDefaultGatewayNoDefaultRoute) {
    EXPECT_FALSE(parse_default_gateway("").has_value());
    EXPECT_FALSE(parse_default_gateway("Iface\tDestination\tGateway\n").has_value());
    // Only the on-link subnet route — no default.
    EXPECT_FALSE(
        parse_default_gateway("Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\n"
                              "eth0\t000011AC\t00000000\t0001\t0\t0\t0\t0000FFFF\t0\t0\t0\n")
            .has_value());
}

TEST(HostAddress, ParseDefaultGatewaySkipsUnusableRows) {
    // An on-link default (gateway 0), a malformed hex gateway, and a short
    // row must all be skipped without giving up on the later valid row.
    const std::string table =
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
        "eth0\t00000000\t00000000\t0001\t0\t0\t0\t00000000\t0\t0\t0\n"
        "eth0\t00000000\tXYZ!!!\t0003\t0\t0\t0\t00000000\t0\t0\t0\n"
        "eth0\t00000000\n"
        "eth1\t00000000\t0100A8C0\t0003\t0\t0\t0\t00000000\t0\t0\t0\n";
    const auto gateway = parse_default_gateway(table);
    ASSERT_TRUE(gateway.has_value());
    EXPECT_EQ(*gateway, "192.168.0.1"); // 0100A8C0 little-endian
}

TEST(HostAddress, ParseDefaultGatewayFirstMatchWins) {
    const std::string table =
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
        "eth0\t00000000\t010011AC\t0003\t0\t0\t0\t00000000\t0\t0\t0\n"
        "eth1\t00000000\t0100A8C0\t0003\t0\t0\t0\t00000000\t0\t0\t0\n";
    EXPECT_EQ(parse_default_gateway(table).value_or(""), "172.17.0.1");
}

/// resolved_host_address falls through to ~/.testcontainers.properties, so
/// these tests need the hermetic temp HOME; the env override is pinned unset
/// so a developer's exported TESTCONTAINERS_HOST_OVERRIDE cannot leak in.
class HostOverrideFile : public tcunit::TempHomeTest {
protected:
    void SetUp() override {
        tcunit::TempHomeTest::SetUp();
        no_override_.emplace("TESTCONTAINERS_HOST_OVERRIDE", std::nullopt);
    }

    void TearDown() override {
        no_override_.reset();
        tcunit::TempHomeTest::TearDown();
    }

private:
    std::optional<ScopedEnv> no_override_;
};

TEST_F(HostOverrideFile, EnvOverrideWinsOnAnyScheme) {
    const ScopedEnv override_env("TESTCONTAINERS_HOST_OVERRIDE", "probe.example.com");
    EXPECT_EQ(resolved_host_address(DockerHost::parse("tcp://1.2.3.4:2375")), "probe.example.com");
    EXPECT_EQ(resolved_host_address(DockerHost::parse("unix:///var/run/docker.sock")),
              "probe.example.com");
}

TEST_F(HostOverrideFile, PropertiesOverrideApplies) {
    set_properties("host.override = tcc.agent.local\n");
    EXPECT_EQ(resolved_host_address(DockerHost::parse("unix:///var/run/docker.sock")),
              "tcc.agent.local");
    EXPECT_EQ(resolved_host_address(DockerHost::parse("tcp://1.2.3.4:2375")), "tcc.agent.local");
}

TEST_F(HostOverrideFile, TcpDaemonUsesHostname) {
    EXPECT_EQ(resolved_host_address(DockerHost::parse("tcp://daemon.example.com:2375")),
              "daemon.example.com");
    EXPECT_EQ(resolved_host_address(DockerHost::parse("https://1.2.3.4:2376")), "1.2.3.4");
}

TEST_F(HostOverrideFile, LocalSocketIsLocalhost) {
    // The test process runs outside any container (no /.dockerenv on dev
    // machines, WSL, or CI VMs), so the in-container gateway branch stays
    // dormant here — its parsing half is pinned by the pure tests above.
    EXPECT_EQ(resolved_host_address(DockerHost::parse("unix:///var/run/docker.sock")), "localhost");
    EXPECT_EQ(resolved_host_address(DockerHost::parse("npipe:////./pipe/docker_engine")),
              "localhost");
}
