#include <gtest/gtest.h>

#include <cstdint>
#include <exception>
#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"

#include "EngineGuard.hpp"
#include "WindowsEngine.hpp"

// Tests in this file (integration; require a Docker daemon — Linux mode for the
// PortGetters suite, Windows mode for the WindowsPortGetters mirror):
//   PortGetters.Ipv4AndDefaultAgree - get_host_port and get_host_port_ipv4 return the same non-zero port; first_mapped_port equals get_host_port.
//   PortGetters.InspectAndRaw - inspect() reports running and the published port; inspect_raw() is non-empty and contains the container id.
//   PortGetters.FirstMappedPicksExposedOrder - first_mapped_port resolves the FIRST exposed port (9090), not the lowest-numbered (8080).
//   WindowsPortGetters.PublishedPortResolvesMappedPort - a Windows container's exposed port is published and get_host_port / get_host_port_ipv4 / first_mapped_port agree on the non-zero mapping; inspect() carries the port key.

using namespace testcontainers;

// alpine won't actually listen on the exposed port, but Docker still PUBLISHES
// it (publish_all_ports), so the host-port getters resolve a real mapped port.
class PortGetters : public ::testing::Test {
protected:
    void SetUp() override {
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

TEST_F(PortGetters, Ipv4AndDefaultAgree) {
    Container c = GenericImage::from_reference("alpine:3.20")
                      .with_exposed_port(tcp(8080))
                      .with_cmd({"sleep", "30"})
                      .start();

    const std::uint16_t def = c.get_host_port(tcp(8080));
    const std::uint16_t v4 = c.get_host_port_ipv4(tcp(8080));
    EXPECT_NE(def, 0);
    EXPECT_EQ(def, v4) << "Docker Desktop publishes the IPv4 binding";
    EXPECT_EQ(c.first_mapped_port(), def);

    // IPv6 publication is daemon-dependent (Docker Desktop on this host does not
    // publish an IPv6 binding by default). Do not require it: either it resolves
    // to a non-zero port, or it throws — both are acceptable / honest here.
    try {
        const std::uint16_t v6 = c.get_host_port_ipv6(tcp(8080));
        EXPECT_NE(v6, 0) << "if an IPv6 binding is published it must be non-zero";
    } catch (const std::exception&) {
        // No IPv6 binding published by this daemon — expected on Docker Desktop.
        SUCCEED() << "daemon did not publish an IPv6 binding (expected on Docker Desktop)";
    }
}

TEST_F(PortGetters, InspectAndRaw) {
    Container c = GenericImage::from_reference("alpine:3.20")
                      .with_exposed_port(tcp(8080))
                      .with_cmd({"sleep", "30"})
                      .start();

    const ContainerInspect info = c.inspect();
    EXPECT_TRUE(info.running);
    EXPECT_NE(info.ports.find("8080/tcp"), info.ports.end())
        << "inspect().ports should contain the published 8080/tcp key";

    const std::string raw = c.inspect_raw();
    EXPECT_FALSE(raw.empty());
    EXPECT_NE(raw.find(c.id()), std::string::npos)
        << "raw inspect body should contain the container id";
}

TEST_F(PortGetters, FirstMappedPicksExposedOrder) {
    // 9090 is declared FIRST but is the HIGHER-numbered port: first_mapped_port
    // must follow the declared order (9090), proving it is not just picking the
    // lowest-numbered published port.
    Container c = GenericImage::from_reference("alpine:3.20")
                      .with_exposed_port(tcp(9090))
                      .with_exposed_port(tcp(8080))
                      .with_cmd({"sleep", "30"})
                      .start();

    EXPECT_EQ(c.first_mapped_port(), c.get_host_port(tcp(9090)));
    EXPECT_NE(c.get_host_port(tcp(9090)), c.get_host_port(tcp(8080)));
}

// The Windows mirror: publication does not need an in-container listener (the
// nat forward is created regardless), so nanoserver + keep-alive suffices.
class WindowsPortGetters : public tcit::WindowsEngineTest {};

TEST_F(WindowsPortGetters, PublishedPortResolvesMappedPort) {
    Container c = nanoserver()
                      .with_exposed_port(tcp(8080))
                      .with_cmd(keep_alive_cmd())
                      .start();

    const std::uint16_t mapped = c.get_host_port(tcp(8080));
    EXPECT_NE(mapped, 0);
    EXPECT_EQ(c.get_host_port_ipv4(tcp(8080)), mapped);
    EXPECT_EQ(c.first_mapped_port(), mapped);

    const ContainerInspect info = c.inspect();
    EXPECT_TRUE(info.running);
    EXPECT_NE(info.ports.find("8080/tcp"), info.ports.end())
        << "inspect().ports should contain the published 8080/tcp key";
}
