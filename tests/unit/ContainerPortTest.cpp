#include <gtest/gtest.h>

#include <vector>

#include "testcontainers/ContainerPort.hpp"

// Tests in this file:
//   ContainerPort.TcpFactory - tcp() builds a port with the Tcp protocol.
//   ContainerPort.UdpFactory - udp() builds a port with the Udp protocol.
//   ContainerPort.SctpFactory - sctp() builds a port with the Sctp protocol.
//   ContainerPort.ToStringFormatsPortAndProto - to_string formats a port as "port/proto" per protocol.
//   ContainerPort.Equality - two ports compare equal iff both port and protocol match.
//   ContainerPort.CopyableAndStorable - a port is copyable and lives in a std::vector unchanged.

using namespace testcontainers;

TEST(ContainerPort, TcpFactory) {
    const ContainerPort p = tcp(6379);
    EXPECT_EQ(p.port, 6379);
    EXPECT_EQ(p.proto, Proto::Tcp);
}

TEST(ContainerPort, UdpFactory) {
    const ContainerPort p = udp(53);
    EXPECT_EQ(p.port, 53);
    EXPECT_EQ(p.proto, Proto::Udp);
}

TEST(ContainerPort, SctpFactory) {
    const ContainerPort p = sctp(9899);
    EXPECT_EQ(p.port, 9899);
    EXPECT_EQ(p.proto, Proto::Sctp);
}

TEST(ContainerPort, ToStringFormatsPortAndProto) {
    EXPECT_EQ(to_string(tcp(6379)), "6379/tcp");
    EXPECT_EQ(to_string(udp(53)), "53/udp");
    EXPECT_EQ(to_string(sctp(9899)), "9899/sctp");
}

TEST(ContainerPort, Equality) {
    EXPECT_EQ(tcp(6379), tcp(6379));
    EXPECT_NE(tcp(6379), udp(6379));       // same port, different proto
    EXPECT_NE(tcp(6379), tcp(6380));       // same proto, different port
    EXPECT_EQ(ContainerPort{80}, tcp(80)); // default proto is Tcp
}

TEST(ContainerPort, CopyableAndStorable) {
    const ContainerPort original = tcp(8080);
    ContainerPort copy = original; // copy-construct
    EXPECT_EQ(copy, original);

    std::vector<ContainerPort> ports;
    ports.push_back(tcp(1));
    ports.push_back(udp(2));
    EXPECT_EQ(ports.size(), 2u);
    EXPECT_EQ(ports[0], tcp(1));
    EXPECT_EQ(ports[1], udp(2));
}
