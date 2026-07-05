#include <gtest/gtest.h>

#include <exception>
#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/Network.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"
#include "WindowsEngine.hpp"

// Tests in this file (integration; require a Docker daemon — Linux mode for the
// Networks suite, Windows mode for the WindowsNetworks mirror):
//   Networks.ResolvesPeerByContainerName - two containers on a user-defined network reach each other by container name (busybox `nc` to a redis peer on the network succeeds).
//   Networks.CreateAndRemove - Network::create makes a real network with a non-empty id/name and remove() is idempotent.
//   Networks.AliasResolvesOnCustomNetwork - a container with a network alias is reachable by that alias from a peer on the same network (getent resolves the alias to an IP).
//   Networks.BuilderCreatesNetwork - Network::builder() with a driver, attachable, and an IPAM subnet creates a real network with a non-empty id/name.
//   Networks.ConnectAttachesRunningContainerWithAlias - Network::connect attaches an already-running container (started WITHOUT with_network) and its runtime alias resolves from a peer on the network.
//   Networks.BuilderInternalGatewayAndLabels - builder internal/gateway/label options land in the created network (asserted via GET /networks/{id}).
//   Networks.StaticIpv4Assigned - with_static_ipv4 on a subnet-bearing network pins the endpoint to exactly the requested address (asserted via container inspect).
//   WindowsNetworks.CreateAndRemove - the same create/remove round-trip on a Windows daemon (the default driver there is "nat").
//   WindowsNetworks.PeerNameRegisteredAndReachable - two nanoserver containers on a user-defined nat network: the daemon registers the peer's container name in DNSNames, and a `ping` of the peer's network IP proves the data path.
//   WindowsNetworks.AliasRegisteredOnCustomNetwork - with_network_alias lands in the endpoint's Aliases/DNSNames, and the alias-bearing peer is reachable by its network IP.
//   WindowsNetworks.BuilderCreatesNetwork - Network::builder() with the "nat" driver and an IPAM subnet creates a real network (no attachable — that is a swarm/overlay concept HNS does not take).

using namespace testcontainers;

// Requires a reachable Docker daemon; skipped if none is available.
class Networks : public ::testing::Test {
protected:
    void SetUp() override {
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

namespace {

/// The container's IPv4 address on `network`, scraped from the raw inspect
/// JSON (ContainerInspect does not model per-network endpoint settings).
/// Assumes the endpoint's "IPAddress" key follows the network-name key in the
/// body (true for every daemon we drive) and that the random network name
/// collides with nothing else in it.
std::string ip_on_network(DockerClient& client, const std::string& id, const std::string& network) {
    const std::string body = client.request("GET", "/containers/" + id + "/json").body;
    const std::size_t net_at = body.find("\"" + network + "\"");
    if (net_at == std::string::npos) {
        return "";
    }
    const std::string marker = "\"IPAddress\":\"";
    const std::size_t ip_at = body.find(marker, net_at);
    if (ip_at == std::string::npos) {
        return "";
    }
    const std::size_t start = ip_at + marker.size();
    return body.substr(start, body.find('"', start) - start);
}

} // namespace

TEST_F(Networks, ResolvesPeerByContainerName) {
    // A user-defined network gives containers DNS resolution by container name.
    Network net = Network::create();
    ASSERT_FALSE(net.name().empty());

    Container redis = GenericImage("redis", "7.2")
                          .with_network(net.name())
                          .with_container_name("redis-srv")
                          .with_exposed_port(tcp(6379))
                          .with_wait(wait_for::stdout_message("Ready to accept connections"))
                          .start();

    // A long-running client container on the same network; no readiness signal,
    // so no wait strategy is needed.
    Container client =
        GenericImage("alpine", "3.20").with_network(net.name()).with_cmd({"sleep", "60"}).start();

    // busybox `nc -z` proves both DNS-by-name and TCP connectivity on the
    // user-defined network.
    const ExecResult res = client.exec({"sh", "-c", "nc -z -w 3 redis-srv 6379"});
    EXPECT_EQ(res.exit_code, 0) << "stdout: " << res.stdout_data << " stderr: " << res.stderr_data;

    // Containers and the network are torn down by RAII at scope exit, in reverse
    // construction order (client, redis, then net).
}

TEST_F(Networks, CreateAndRemove) {
    Network net = Network::create("tc-test-network-explicit");
    EXPECT_EQ(net.name(), "tc-test-network-explicit");
    EXPECT_FALSE(net.id().empty());

    net.remove();
    EXPECT_NO_THROW(net.remove()); // idempotent
}

TEST_F(Networks, AliasResolvesOnCustomNetwork) {
    // A network alias gives a container an extra DNS name on the network. Declare
    // the network FIRST so RAII tears the containers down before it (a network
    // can't be removed while containers are still attached).
    Network net = Network::create();
    ASSERT_FALSE(net.name().empty());

    // A long-running container that advertises the "db" alias on the network. No
    // readiness signal is produced by `sleep`, so no wait strategy is needed.
    Container db = GenericImage("alpine", "3.20")
                       .with_network(net.name())
                       .with_network_alias("db")
                       .with_cmd({"sleep", "60"})
                       .start();

    // A second container on the same network resolves the alias over the custom
    // network's DNS; `getent hosts db` exits 0 and prints "<ip> db".
    Container client =
        GenericImage("alpine", "3.20").with_network(net.name()).with_cmd({"sleep", "60"}).start();

    const ExecResult res = client.exec({"getent", "hosts", "db"});
    EXPECT_EQ(res.exit_code, 0) << "stdout: " << res.stdout_data << " stderr: " << res.stderr_data;
    EXPECT_FALSE(res.stdout_data.empty()) << "alias 'db' did not resolve to an address";

    // Containers and the network are torn down by RAII at scope exit, in reverse
    // construction order (client, db, then net).
}

TEST_F(Networks, BuilderCreatesNetwork) {
    Network n = Network::builder()
                    .with_driver("bridge")
                    .with_attachable()
                    .with_subnet("172.31.251.0/24")
                    .create();
    EXPECT_FALSE(n.id().empty());
    EXPECT_FALSE(n.name().empty());

    // RAII removes the network at scope exit.
}

TEST_F(Networks, ConnectAttachesRunningContainerWithAlias) {
    // Declare the network FIRST so RAII tears the containers down before it
    // (removal auto-detaches their endpoints).
    Network net = Network::create();
    ASSERT_FALSE(net.name().empty());

    // Started WITHOUT with_network: it sits on the default bridge only.
    Container late = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();
    Container peer =
        GenericImage("alpine", "3.20").with_network(net.name()).with_cmd({"sleep", "60"}).start();

    // Attach the RUNNING container to the network with a runtime alias.
    net.connect(late.id(), {"late-alias"});

    // The alias resolves from a peer on the same network — proving the attach
    // (and its alias) took effect daemon-side, not just returned 200.
    const ExecResult res = peer.exec({"getent", "hosts", "late-alias"});
    EXPECT_EQ(res.exit_code, 0) << "stdout: " << res.stdout_data << " stderr: " << res.stderr_data;
    EXPECT_FALSE(res.stdout_data.empty()) << "alias 'late-alias' did not resolve";
}

TEST_F(Networks, BuilderInternalGatewayAndLabels) {
    // Internal + gateway + label are not modelled by any typed inspect, so
    // assert them in the raw network-inspect body.
    Network n = Network::builder()
                    .with_driver("bridge")
                    .with_subnet("172.31.253.0/24")
                    .with_gateway("172.31.253.1")
                    .with_internal()
                    .with_label("tc-test-label", "yes")
                    .create();

    DockerClient client = DockerClient::from_environment();
    const std::string body = client.request("GET", "/networks/" + n.id()).body;
    EXPECT_NE(body.find("\"Internal\":true"), std::string::npos) << body.substr(0, 512);
    EXPECT_NE(body.find("172.31.253.1"), std::string::npos) << body.substr(0, 512);
    EXPECT_NE(body.find("tc-test-label"), std::string::npos) << body.substr(0, 512);

    // RAII removes the network at scope exit.
}

TEST_F(Networks, StaticIpv4Assigned) {
    // A fixed address needs a user-defined network whose subnet contains it
    // (distinct subnet from the other suites so shared daemons never collide).
    Network net = Network::builder().with_subnet("172.31.254.0/24").create();
    ASSERT_FALSE(net.name().empty());

    Container c = GenericImage("alpine", "3.20")
                      .with_network(net.name())
                      .with_static_ipv4("172.31.254.10")
                      .with_cmd({"sleep", "60"})
                      .start();

    // The endpoint must carry exactly the requested address, not a pool pick.
    DockerClient dc = DockerClient::from_environment();
    EXPECT_EQ(ip_on_network(dc, c.id(), net.name()), "172.31.254.10");

    // The container and the network are torn down by RAII at scope exit.
}

// The Windows mirror: user-defined networks use the "nat" driver (the Windows
// daemon's default). Unlike the Linux tests, these do NOT resolve peers by
// name in-container: whether HNS actually SERVES single-label DNS names to a
// process-isolated container is environment-dependent (a host DNS-suffix
// search list breaks it, observed first-hand), and that resolver belongs to
// Windows, not to this client. What IS our contract — the daemon registering
// the name/alias on the endpoint (DNSNames/Aliases) and the network carrying
// traffic — is asserted directly: inspect JSON + `ping` of the peer's network
// IP (nanoserver ships no netcat/getent, so ICMP is the reachability probe).
class WindowsNetworks : public tcit::WindowsEngineTest {};

TEST_F(WindowsNetworks, CreateAndRemove) {
    Network net = Network::create("tc-test-network-win-explicit");
    EXPECT_EQ(net.name(), "tc-test-network-win-explicit");
    EXPECT_FALSE(net.id().empty());

    net.remove();
    EXPECT_NO_THROW(net.remove()); // idempotent
}

TEST_F(WindowsNetworks, PeerNameRegisteredAndReachable) {
    Network net = Network::create();
    ASSERT_FALSE(net.name().empty());

    Container srv = nanoserver()
                        .with_network(net.name())
                        .with_container_name("tc-win-peer-srv")
                        .with_cmd(keep_alive_cmd())
                        .start();

    Container client = nanoserver().with_network(net.name()).with_cmd(keep_alive_cmd()).start();

    // The daemon must register the container name as a DNS name on the
    // user-defined network's endpoint (this is what backs name resolution).
    DockerClient dc = DockerClient::from_environment();
    const std::string inspect = dc.request("GET", "/containers/" + srv.id() + "/json").body;
    const std::size_t dns_at = inspect.find("\"DNSNames\"");
    ASSERT_NE(dns_at, std::string::npos) << "no DNSNames in inspect: " << inspect.substr(0, 512);
    EXPECT_NE(inspect.find("tc-win-peer-srv", dns_at), std::string::npos)
        << "container name not registered for DNS on the network";

    // An ICMP round-trip to the peer's address on the network proves the
    // user-defined network carries traffic between the two containers. Three
    // echoes: ping exits 0 if ANY reply arrives, so one dropped packet on a
    // loaded CI runner cannot flake the test.
    const std::string ip = ip_on_network(dc, srv.id(), net.name());
    ASSERT_FALSE(ip.empty()) << "no IPv4 address on " << net.name();
    const ExecResult res = client.exec({"cmd", "/c", "ping -4 -n 3 -w 2000 " + ip});
    EXPECT_EQ(res.exit_code, 0) << "stdout: " << res.stdout_data << " stderr: " << res.stderr_data;

    // Containers and the network are torn down by RAII at scope exit, in reverse
    // construction order (client, srv, then net).
}

TEST_F(WindowsNetworks, AliasRegisteredOnCustomNetwork) {
    // Declare the network FIRST so RAII tears the containers down before it.
    Network net = Network::create();
    ASSERT_FALSE(net.name().empty());

    Container db = nanoserver()
                       .with_network(net.name())
                       .with_network_alias("tc-win-db")
                       .with_cmd(keep_alive_cmd())
                       .start();

    Container client = nanoserver().with_network(net.name()).with_cmd(keep_alive_cmd()).start();

    // The alias must land on the endpoint's Aliases (anchor the search there
    // so a stray match elsewhere in the body can never fake a pass).
    DockerClient dc = DockerClient::from_environment();
    const std::string inspect = dc.request("GET", "/containers/" + db.id() + "/json").body;
    const std::size_t aliases_at = inspect.find("\"Aliases\"");
    ASSERT_NE(aliases_at, std::string::npos) << "no Aliases in inspect: " << inspect.substr(0, 512);
    EXPECT_NE(inspect.find("\"tc-win-db\"", aliases_at), std::string::npos)
        << "alias not registered on the network endpoint";

    // And the alias-bearing peer is reachable over the network (three echoes:
    // any single reply passes, so one dropped packet cannot flake the test).
    const std::string ip = ip_on_network(dc, db.id(), net.name());
    ASSERT_FALSE(ip.empty()) << "no IPv4 address on " << net.name();
    const ExecResult res = client.exec({"cmd", "/c", "ping -4 -n 3 -w 2000 " + ip});
    EXPECT_EQ(res.exit_code, 0) << "stdout: " << res.stdout_data << " stderr: " << res.stderr_data;

    // Containers and the network are torn down by RAII at scope exit, in reverse
    // construction order (client, db, then net).
}

TEST_F(WindowsNetworks, BuilderCreatesNetwork) {
    // No with_attachable(): attachable is a swarm/overlay flag; HNS nat
    // networks reject it. The subnet is distinct from the Linux builder test's
    // so the two suites can never collide on a shared daemon.
    Network n = Network::builder().with_driver("nat").with_subnet("172.31.252.0/24").create();
    EXPECT_FALSE(n.id().empty());
    EXPECT_FALSE(n.name().empty());

    // RAII removes the network at scope exit.
}
