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
