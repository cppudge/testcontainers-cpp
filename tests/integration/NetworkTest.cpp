#include <gtest/gtest.h>

#include <exception>
#include <optional>
#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/Network.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"
#include "RandomHex.hpp"
#include "TestEnv.hpp"
#include "WindowsEngine.hpp"

// Tests in this file (integration; require a Docker daemon — Linux mode for the
// Networks suite, Windows mode for the WindowsNetworks mirror):
//   Networks.ResolvesPeerByContainerName - two containers on a user-defined network reach each other by container name (busybox `nc` to a redis peer on the network succeeds).
//   Networks.CreateAndRemove - Network::create makes a real network with a non-empty id/name and remove() is idempotent.
//   Networks.AliasResolvesOnCustomNetwork - a container with a network alias is reachable by that alias from a peer on the same network (getent resolves the alias to an IP).
//   Networks.BuilderCreatesNetwork - Network::builder() with a driver, attachable, and an IPAM subnet creates a real network with a non-empty id/name.
//   Networks.ConnectAttachesRunningContainerWithAlias - Network::connect attaches an already-running container (started WITHOUT with_network) and its runtime alias resolves from a peer on the network.
//   Networks.BuilderInternalGatewayAndLabels - builder internal/gateway/label options land in the created network (asserted via the typed Network::inspect()).
//   Networks.BuilderMultiPoolIpamRoundTrip - with_ipam_pool x2 (a full IPv4 pool with IPRange/aux addresses + an IPv6 pool under with_enable_ipv6) round-trips through the typed inspect, and an attached container's address is drawn from the IPv4 pool's IPRange (the daemon acted on the field, not just echoed it).
//   Networks.StaticIpv4Assigned - with_static_ipv4 on a subnet-bearing network pins the endpoint to exactly the requested address (asserted via container inspect).
//   Networks.ListNetworksFindsByLabel - list_networks with a label filter returns exactly the matching network (id/name/labels); the name filter finds it too (substring match daemon-side, exact match post-filtered by the caller).
//   Networks.BuilderWithReuseAdoptsAcrossHandles - with reuse enabled globally: the first create makes a persistent reuse network (managed-by + reuse-hash labels, NO session label), a second identical create adopts the same network after the first handle dropped, and a same-name create with a DIFFERENT config throws instead of making an ambiguous duplicate.
//   Networks.BuilderWithReuseDisabledDegrades - with reuse NOT enabled globally, with_reuse degrades to a normal session-labeled network: two creates make two distinct auto-removed networks.
//   Networks.BuilderWithReuseRequiresName - with reuse enabled globally, with_reuse without with_name throws (a generated name would never match across runs).
//   Networks.InspectReportsConfigAndContainers - net.inspect() reflects the created driver/IPAM pool/labels and lists an attached container's endpoint; the static Network::inspect(name) resolves the same network; inspect_raw() returns the raw body.
//   Networks.KeepReleasesRemovalOwnership - keep() makes the handle persistent (neither remove() nor drop removes the network; cleaned up manually), while keep(false) re-arms removal so the drop removes it after all.
//   WindowsNetworks.CreateAndRemove - the same create/remove round-trip on a Windows daemon (the default driver there is "nat").
//   WindowsNetworks.PeerNameRegisteredAndReachable - two nanoserver containers on a user-defined nat network: the daemon registers the peer's container name in DNSNames, and a `ping` of the peer's network IP proves the data path.
//   WindowsNetworks.AliasRegisteredOnCustomNetwork - with_network_alias lands in the endpoint's Aliases/DNSNames, and the alias-bearing peer is reachable by its network IP.
//   WindowsNetworks.BuilderCreatesNetwork - Network::builder() with the "nat" driver and an IPAM subnet creates a real network (no attachable — that is a swarm/overlay concept HNS does not take).
//   WindowsNetworks.InspectReportsDriverAndContainers - net.inspect() on a nat network reflects the driver and IPAM pool and lists an attached container's endpoint; the static Network::inspect(name) resolves the same network.

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
/// The search is anchored at the NetworkSettings "Networks" object: the
/// network NAME also appears earlier as HostConfig.NetworkMode, and on
/// daemons that still emit the legacy (empty) NetworkSettings.IPAddress
/// field an unanchored search would scrape that instead. Assumes the
/// endpoint's "IPAddress" key follows the network-name key (true for every
/// daemon we drive) and that the random network name collides with nothing
/// else inside "Networks".
std::string ip_on_network(DockerClient& client, const std::string& id, const std::string& network) {
    const std::string body = client.request("GET", "/containers/" + id + "/json").body;
    const std::size_t networks_at = body.find("\"Networks\"");
    if (networks_at == std::string::npos) {
        return "";
    }
    const std::size_t net_at = body.find("\"" + network + "\"", networks_at);
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
                          .with_network(net)
                          .with_container_name("redis-srv")
                          .with_exposed_port(tcp(6379))
                          .with_wait(wait_for::stdout_message("Ready to accept connections"))
                          .start();

    // A long-running client container on the same network; no readiness signal,
    // so no wait strategy is needed.
    Container client =
        GenericImage("alpine", "3.20").with_network(net).with_cmd({"sleep", "60"}).start();

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
                       .with_network(net)
                       .with_network_alias("db")
                       .with_cmd({"sleep", "60"})
                       .start();

    // A second container on the same network resolves the alias over the custom
    // network's DNS; `getent hosts db` exits 0 and prints "<ip> db".
    Container client =
        GenericImage("alpine", "3.20").with_network(net).with_cmd({"sleep", "60"}).start();

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
        GenericImage("alpine", "3.20").with_network(net).with_cmd({"sleep", "60"}).start();

    // Attach the RUNNING container to the network with a runtime alias.
    net.connect(late.id(), {"late-alias"});

    // The alias resolves from a peer on the same network — proving the attach
    // (and its alias) took effect daemon-side, not just returned 200.
    const ExecResult res = peer.exec({"getent", "hosts", "late-alias"});
    EXPECT_EQ(res.exit_code, 0) << "stdout: " << res.stdout_data << " stderr: " << res.stderr_data;
    EXPECT_FALSE(res.stdout_data.empty()) << "alias 'late-alias' did not resolve";
}

TEST_F(Networks, BuilderInternalGatewayAndLabels) {
    Network n = Network::builder()
                    .with_driver("bridge")
                    .with_subnet("172.31.253.0/24")
                    .with_gateway("172.31.253.1")
                    .with_internal()
                    .with_label("tc-test-label", "yes")
                    .create();

    // The typed inspect models all three options directly.
    const NetworkInspect info = n.inspect();
    EXPECT_TRUE(info.internal);
    ASSERT_EQ(info.ipam_pools.size(), 1u);
    EXPECT_EQ(info.ipam_pools[0].gateway, "172.31.253.1");
    ASSERT_EQ(info.labels.count("tc-test-label"), 1u);
    EXPECT_EQ(info.labels.at("tc-test-label"), "yes");

    // RAII removes the network at scope exit.
}

TEST_F(Networks, BuilderMultiPoolIpamRoundTrip) {
    // A subnet distinct from every other suite's so shared daemons never collide.
    NetworkIpamPool v4;
    v4.subnet = "172.31.247.0/24";
    v4.ip_range = "172.31.247.128/25";
    v4.gateway = "172.31.247.1";
    v4.aux_addresses = {{"router", "172.31.247.2"}};
    NetworkIpamPool v6;
    v6.subnet = "fd00:7c47::/64";
    v6.gateway = "fd00:7c47::1";

    // The bridge driver takes at most one pool per family, so "multiple pools"
    // on it means IPv4 + IPv6.
    Network net =
        Network::builder().with_enable_ipv6().with_ipam_pool(v4).with_ipam_pool(v6).create();

    // Every pool field echoes back through the typed inspect (IPv4 pools come
    // first in moby's inspect order).
    const NetworkInspect info = net.inspect();
    EXPECT_TRUE(info.enable_ipv6);
    ASSERT_EQ(info.ipam_pools.size(), 2u);
    EXPECT_EQ(info.ipam_pools[0].subnet, "172.31.247.0/24");
    EXPECT_EQ(info.ipam_pools[0].ip_range, "172.31.247.128/25");
    EXPECT_EQ(info.ipam_pools[0].gateway, "172.31.247.1");
    ASSERT_EQ(info.ipam_pools[0].aux_addresses.size(), 1u);
    EXPECT_EQ(info.ipam_pools[0].aux_addresses[0].first, "router");
    EXPECT_EQ(info.ipam_pools[0].aux_addresses[0].second, "172.31.247.2");
    EXPECT_EQ(info.ipam_pools[1].subnet, "fd00:7c47::/64");
    EXPECT_EQ(info.ipam_pools[1].gateway, "fd00:7c47::1");

    // The echo proves the field NAMES landed; an attached container drawing its
    // address from the upper half-subnet proves IPRange took effect.
    Container c =
        GenericImage("alpine", "3.20").with_network(net).with_cmd({"sleep", "60"}).start();
    DockerClient dc = DockerClient::from_environment();
    const std::string ip = ip_on_network(dc, c.id(), net.name());
    ASSERT_TRUE(ip.starts_with("172.31.247.")) << ip;
    EXPECT_GE(std::stoi(ip.substr(ip.rfind('.') + 1)), 128) << ip;

    // The container and the network are torn down by RAII at scope exit.
}

TEST_F(Networks, StaticIpv4Assigned) {
    // A fixed address needs a user-defined network whose subnet contains it
    // (distinct subnet from the other suites so shared daemons never collide).
    Network net = Network::builder().with_subnet("172.31.254.0/24").create();
    ASSERT_FALSE(net.name().empty());

    Container c = GenericImage("alpine", "3.20")
                      .with_network(net)
                      .with_static_ipv4("172.31.254.10")
                      .with_cmd({"sleep", "60"})
                      .start();

    // The endpoint must carry exactly the requested address, not a pool pick.
    DockerClient dc = DockerClient::from_environment();
    EXPECT_EQ(ip_on_network(dc, c.id(), net.name()), "172.31.254.10");

    // The container and the network are torn down by RAII at scope exit.
}

TEST_F(Networks, ListNetworksFindsByLabel) {
    // A unique label value so stale resources from earlier runs can't collide.
    const std::string marker = "list-it-" + detail::random_hex(8);
    Network tagged = Network::builder().with_label("tc-list-marker", marker).create();
    Network other = Network::create();

    DockerClient dc = DockerClient::from_environment();
    const auto by_label = dc.list_networks({{"label", "tc-list-marker=" + marker}});
    ASSERT_EQ(by_label.size(), 1u);
    EXPECT_EQ(by_label[0].id, tagged.id());
    EXPECT_EQ(by_label[0].name, tagged.name());
    EXPECT_EQ(by_label[0].labels.at("tc-list-marker"), marker);

    // The name filter matches substrings daemon-side; exact-name callers
    // post-filter (as the reuse lookup does).
    const auto by_name = dc.list_networks({{"name", tagged.name()}});
    bool found = false;
    for (const auto& n : by_name) {
        found = found || n.id == tagged.id();
    }
    EXPECT_TRUE(found) << "name filter did not return the network";

    // Both networks are torn down by RAII at scope exit.
}

TEST_F(Networks, BuilderWithReuseAdoptsAcrossHandles) {
    const tctest::ScopedEnv enable("TESTCONTAINERS_REUSE_ENABLE", "true");

    // A unique name so a stale reuse network from an earlier run can't match.
    const std::string name = "tc-reuse-net-" + detail::random_hex(8);
    const Network::Builder base =
        Network::builder().with_name(name).with_label("tc-reuse-it", "yes").with_reuse();

    std::string created_id;
    try {
        {
            Network first = base.create();
            created_id = first.id();
            ASSERT_FALSE(created_id.empty());
            EXPECT_TRUE(first.is_persistent()); // reuse handles do not auto-remove

            // Reuse networks carry managed-by + the reuse hash and NO session
            // label (Ryuk must not reap what has to survive the run).
            const NetworkInspect info = first.inspect();
            EXPECT_EQ(info.labels.count("org.testcontainers.reuse.hash"), 1u);
            EXPECT_EQ(info.labels.count("org.testcontainers.managed-by"), 1u);
            EXPECT_EQ(info.labels.count("org.testcontainers.session-id"), 0u);
        } // the persistent handle dropped without removing the network

        // A second identical create must adopt the surviving network.
        {
            Network second = base.create();
            EXPECT_EQ(second.id(), created_id) << "second create did not adopt";
            EXPECT_TRUE(second.is_persistent());
        }

        // Same name, DIFFERENT config: creating would make an ambiguous
        // same-named duplicate (Docker does not enforce unique network names),
        // so create() must refuse.
        Network::Builder changed = base;
        changed.with_label("tc-reuse-extra", "1");
        try {
            Network dup = changed.create();
            ADD_FAILURE() << "same-name different-config create did not throw";
            dup.keep(false); // arm removal so the duplicate is cleaned up anyway
        } catch (const DockerError&) {
            // expected
        }
    } catch (...) {
        // Manual cleanup: the reuse network is persistent and NOT reaped, so
        // no handle will remove it — we must, even on failure.
        if (!created_id.empty()) {
            try {
                DockerClient::from_environment().remove_network(created_id);
            } catch (...) {
                // Best-effort: rethrowing the real failure matters more.
            }
        }
        throw;
    }

    // Manual cleanup (see above): the persistent handles removed nothing.
    ASSERT_FALSE(created_id.empty());
    EXPECT_NO_THROW(DockerClient::from_environment().remove_network(created_id));
}

TEST_F(Networks, BuilderWithReuseDisabledDegrades) {
    const tctest::ScopedEnv disable("TESTCONTAINERS_REUSE_ENABLE", std::nullopt);

    // With reuse NOT enabled globally, with_reuse degrades to a normal
    // (session-labeled, auto-removed) network — and needs no fixed name.
    Network a = Network::builder().with_reuse().create();
    Network b = Network::builder().with_reuse().create();
    EXPECT_NE(a.id(), b.id()) << "reuse should be inactive when not enabled globally";
    EXPECT_FALSE(a.is_persistent());
    EXPECT_FALSE(b.is_persistent());

    const NetworkInspect info = a.inspect();
    EXPECT_EQ(info.labels.count("org.testcontainers.session-id"), 1u);
    EXPECT_EQ(info.labels.count("org.testcontainers.reuse.hash"), 0u);

    // Both networks are torn down by RAII at scope exit.
}

TEST_F(Networks, BuilderWithReuseRequiresName) {
    const tctest::ScopedEnv enable("TESTCONTAINERS_REUSE_ENABLE", "true");

    // An active reuse request without a fixed name can never match across
    // runs — create() must refuse rather than quietly make throwaway networks.
    EXPECT_THROW(Network::builder().with_reuse().create(), DockerError);
}

TEST_F(Networks, KeepReleasesRemovalOwnership) {
    // keep() releases removal ownership: neither an explicit remove() nor the
    // handle's drop touches the network; keep(false) re-arms removal.
    std::string kept_id;
    std::string unkept_name;
    {
        Network kept = Network::create();
        kept_id = kept.id();
        EXPECT_FALSE(kept.is_persistent());
        kept.keep();
        EXPECT_TRUE(kept.is_persistent());
        kept.remove(); // on a kept handle: releases ownership, removes nothing

        Network unkept = Network::create();
        unkept_name = unkept.name();
        unkept.keep();
        unkept.keep(false); // re-armed: the drop below removes it after all
        EXPECT_FALSE(unkept.is_persistent());
    } // drop: must remove ONLY the re-armed network

    // The kept network survived both remove() and the drop; cleaning it up is
    // now this caller's responsibility.
    EXPECT_EQ(Network::inspect(kept_id).id, kept_id);
    DockerClient::from_environment().remove_network(kept_id);

    // The re-armed one is gone.
    EXPECT_THROW(Network::inspect(unkept_name), NotFoundError);
}

TEST_F(Networks, InspectReportsConfigAndContainers) {
    // A subnet distinct from every other suite's so shared daemons never collide.
    Network net = Network::builder()
                      .with_driver("bridge")
                      .with_subnet("172.31.249.0/24")
                      .with_gateway("172.31.249.1")
                      .with_label("tc-inspect-label", "yes")
                      .create();

    Container c =
        GenericImage("alpine", "3.20").with_network(net).with_cmd({"sleep", "60"}).start();

    // The instance method reflects the created configuration...
    const NetworkInspect info = net.inspect();
    EXPECT_EQ(info.id, net.id());
    EXPECT_EQ(info.name, net.name());
    EXPECT_EQ(info.driver, "bridge");
    ASSERT_EQ(info.ipam_pools.size(), 1u);
    EXPECT_EQ(info.ipam_pools[0].subnet, "172.31.249.0/24");
    EXPECT_EQ(info.ipam_pools[0].gateway, "172.31.249.1");
    ASSERT_EQ(info.labels.count("tc-inspect-label"), 1u);
    EXPECT_EQ(info.labels.at("tc-inspect-label"), "yes");

    // ...and lists the attached container's endpoint, its address (CIDR form)
    // drawn from the pool above.
    ASSERT_EQ(info.containers.count(c.id()), 1u);
    EXPECT_TRUE(info.containers.at(c.id()).ipv4_address.starts_with("172.31.249."))
        << info.containers.at(c.id()).ipv4_address;

    // The static lookup takes a name (or id) and needs no Network handle.
    const NetworkInspect by_name = Network::inspect(net.name());
    EXPECT_EQ(by_name.id, net.id());

    // The raw escape hatch returns the same inspect body, unparsed.
    EXPECT_NE(net.inspect_raw().find("\"Id\""), std::string::npos);

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
                        .with_network(net)
                        .with_container_name("tc-win-peer-srv")
                        .with_cmd(keep_alive_cmd())
                        .start();

    Container client = nanoserver().with_network(net).with_cmd(keep_alive_cmd()).start();

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
                       .with_network(net)
                       .with_network_alias("tc-win-db")
                       .with_cmd(keep_alive_cmd())
                       .start();

    Container client = nanoserver().with_network(net).with_cmd(keep_alive_cmd()).start();

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

TEST_F(WindowsNetworks, InspectReportsDriverAndContainers) {
    // The "nat" driver; a subnet distinct from every other suite's so shared
    // daemons never collide.
    Network net = Network::builder().with_driver("nat").with_subnet("172.31.248.0/24").create();

    Container c = nanoserver().with_network(net).with_cmd(keep_alive_cmd()).start();

    const NetworkInspect info = net.inspect();
    EXPECT_EQ(info.id, net.id());
    EXPECT_EQ(info.name, net.name());
    EXPECT_EQ(info.driver, "nat");
    ASSERT_EQ(info.ipam_pools.size(), 1u);
    EXPECT_EQ(info.ipam_pools[0].subnet, "172.31.248.0/24");

    // HNS fills the Containers map exactly like a Linux daemon: one endpoint
    // per attached container, its address in CIDR form from the pool above.
    ASSERT_EQ(info.containers.count(c.id()), 1u);
    EXPECT_TRUE(info.containers.at(c.id()).ipv4_address.starts_with("172.31.248."))
        << info.containers.at(c.id()).ipv4_address;

    // The static lookup takes a name (or id) and needs no Network handle.
    const NetworkInspect by_name = Network::inspect(net.name());
    EXPECT_EQ(by_name.id, net.id());

    // The container and the network are torn down by RAII at scope exit.
}
