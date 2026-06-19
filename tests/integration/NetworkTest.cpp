#include <gtest/gtest.h>

#include <exception>
#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/Network.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon):
//   Networks.ResolvesPeerByContainerName - two containers on a user-defined network reach each other by container name (busybox `nc` to a redis peer on the network succeeds).
//   Networks.CreateAndRemove - Network::create makes a real network with a non-empty id/name and remove() is idempotent.
//   Networks.AliasResolvesOnCustomNetwork - a container with a network alias is reachable by that alias from a peer on the same network (getent resolves the alias to an IP).
//   Networks.BuilderCreatesNetwork - Network::builder() with a driver, attachable, and an IPAM subnet creates a real network with a non-empty id/name.

using namespace testcontainers;

// Requires a reachable Docker daemon; skipped if none is available.
class Networks : public ::testing::Test {
protected:
    void SetUp() override {
        if (auto why = tcit::linux_engine_unavailable()) {
            GTEST_SKIP() << *why;
        }
    }
};

TEST_F(Networks, ResolvesPeerByContainerName) {
    // A user-defined network gives containers DNS resolution by container name.
    Network net = Network::create();
    ASSERT_FALSE(net.name().empty());

    Container redis =
        GenericImage("redis", "7.2")
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
    EXPECT_EQ(res.exit_code, 0) << "stdout: " << res.stdout_data
                                << " stderr: " << res.stderr_data;

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
    EXPECT_EQ(res.exit_code, 0) << "stdout: " << res.stdout_data
                               << " stderr: " << res.stderr_data;
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
