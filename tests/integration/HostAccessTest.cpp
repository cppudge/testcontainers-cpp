#include <gtest/gtest.h>

#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/GenericImage.hpp"

// Tests in this file (integration):
//   [TC_HOST_PORT_FORWARDING builds — the default; require a Docker daemon, Linux engine only (the sshd sidecar is a Linux image)]
//   HostAccess.ContainerReachesHostServiceOnDefaultBridge - a container created with with_exposed_host_port fetches a response from an HTTP server running in the test process via host.testcontainers.internal (default bridge network).
//   HostAccess.ContainerReachesHostServiceOnCustomNetwork - the same through a user-defined network (the sidecar is joined to it on demand), and the network is really REMOVED afterwards (teardown detaches the sidecar; a leak would be swallowed by Network::drop).
//   HostAccess.TwoHostPortsThroughOneSidecar - one container exposes two host ports; both are served through the single process-wide sidecar/tunnel.
//   [TC_HOST_PORT_FORWARDING=OFF builds; no daemon required — the refusal fires before any daemon interaction]
//   HostAccess.DisabledBuildThrowsClearError - start() of an image with with_exposed_host_port throws a DockerError naming the TC_HOST_PORT_FORWARDING build option (before creating anything).

using namespace testcontainers;

#if defined(TC_HOST_PORT_FORWARDING)

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>

#include <boost/asio.hpp>

#include "testcontainers/ExecResult.hpp"
#include "testcontainers/Network.hpp"

#include "EngineGuard.hpp"

// Skipped without a Linux-containers daemon (the sshd sidecar image is Linux).
class HostAccess : public ::testing::Test {
protected:
    void SetUp() override {
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

/// A minimal host-side HTTP server on 127.0.0.1:<ephemeral port>: every
/// connection gets a 200 response carrying `body`, then the connection is
/// closed. Accepting is ASYNC on purpose: a blocking accept() on Linux is NOT
/// woken by closing the acceptor from another thread (it is on Windows, which
/// hid exactly this hang), so teardown stops the io_context instead.
class HostHttpServer {
public:
    explicit HostHttpServer(std::string body)
        : body_(std::move(body)),
          acceptor_(io_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
        port_ = acceptor_.local_endpoint().port();
        accept_next();
        thread_ = std::thread([this] { io_.run(); });
    }

    HostHttpServer(const HostHttpServer&) = delete;
    HostHttpServer& operator=(const HostHttpServer&) = delete;

    ~HostHttpServer() {
        io_.stop(); // parked async_accept is abandoned; run() returns
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::uint16_t port() const noexcept { return port_; }
    int connections() const noexcept { return connections_.load(); }

private:
    void accept_next() {
        auto sock = std::make_shared<tcp::socket>(io_);
        acceptor_.async_accept(*sock, [this, sock](const boost::system::error_code& ec) {
            if (ec) {
                return; // acceptor closed / io stopped
            }
            connections_.fetch_add(1);
            serve(*sock);
            accept_next();
        });
    }

    /// Serve one connection synchronously on the io thread — fine for a test
    /// helper: clients (wget through the tunnel) finish within the test, so
    /// teardown never races an in-flight exchange.
    void serve(tcp::socket& sock) {
        boost::system::error_code ec;
        // Read whatever arrived of the request (one read is enough for a
        // one-packet GET), then answer and close.
        char buf[4096];
        sock.read_some(asio::buffer(buf), ec);
        const std::string response = "HTTP/1.0 200 OK\r\n"
                                     "Content-Type: text/plain\r\n"
                                     "Content-Length: " +
                                     std::to_string(body_.size()) + "\r\n\r\n" + body_;
        asio::write(sock, asio::buffer(response), ec);
        sock.shutdown(tcp::socket::shutdown_send, ec);
        sock.close(ec);
    }

    std::string body_;
    asio::io_context io_;
    tcp::acceptor acceptor_;
    std::uint16_t port_ = 0;
    std::atomic<int> connections_{0};
    std::thread thread_;
};

/// wget the given host port via the in-container alias and return the result.
ExecResult fetch_from_host(const Container& c, std::uint16_t port) {
    return c.exec({"wget", "-q", "-T", "10", "-O", "-",
                   "http://host.testcontainers.internal:" + std::to_string(port) + "/"});
}

} // namespace

TEST_F(HostAccess, ContainerReachesHostServiceOnDefaultBridge) {
    HostHttpServer server("hello-from-the-host");

    Container c = GenericImage("alpine", "3.20")
                      .with_exposed_host_port(server.port())
                      .with_cmd({"sleep", "120"})
                      .start();

    const ExecResult res = fetch_from_host(c, server.port());
    EXPECT_EQ(res.exit_code, 0) << "stdout: " << res.stdout_data << " stderr: " << res.stderr_data;
    EXPECT_NE(res.stdout_data.find("hello-from-the-host"), std::string::npos)
        << "stdout: " << res.stdout_data;
    EXPECT_GE(server.connections(), 1);
}

TEST_F(HostAccess, ContainerReachesHostServiceOnCustomNetwork) {
    // The sidecar starts on the default bridge; wiring a container on a
    // user-defined network must join the sidecar to that network and point the
    // alias at its address THERE.
    HostHttpServer server("hello-across-networks");
    std::string net_id;
    {
        Network net = Network::create();
        net_id = net.id();

        Container c = GenericImage("alpine", "3.20")
                          .with_network(net)
                          .with_exposed_host_port(server.port())
                          .with_cmd({"sleep", "120"})
                          .start();

        const ExecResult res = fetch_from_host(c, server.port());
        EXPECT_EQ(res.exit_code, 0)
            << "stdout: " << res.stdout_data << " stderr: " << res.stderr_data;
        EXPECT_NE(res.stdout_data.find("hello-across-networks"), std::string::npos)
            << "stdout: " << res.stdout_data;

        // RAII order: the container is torn down first; Network teardown then
        // detaches the process-wide sidecar (HostPortForwarder::release_network)
        // before removing the network — a network with active endpoints cannot
        // be removed, and Network::drop swallows the failure.
    }

    // ...which is why the removal must be asserted explicitly: were the
    // sidecar still attached, the network would silently leak.
    DockerClient client = DockerClient::from_environment();
    const Response res = client.request("GET", "/networks/" + net_id);
    EXPECT_EQ(res.status_code, 404) << "network leaked after teardown: " << res.body;
}

TEST_F(HostAccess, TwoHostPortsThroughOneSidecar) {
    // Two distinct host services, one container: both ports ride the single
    // process-wide sidecar and SSH session.
    HostHttpServer first("first-service");
    HostHttpServer second("second-service");

    Container c = GenericImage("alpine", "3.20")
                      .with_exposed_host_port(first.port())
                      .with_exposed_host_port(second.port())
                      .with_cmd({"sleep", "120"})
                      .start();

    const ExecResult res1 = fetch_from_host(c, first.port());
    EXPECT_EQ(res1.exit_code, 0);
    EXPECT_NE(res1.stdout_data.find("first-service"), std::string::npos)
        << "stdout: " << res1.stdout_data;

    const ExecResult res2 = fetch_from_host(c, second.port());
    EXPECT_EQ(res2.exit_code, 0);
    EXPECT_NE(res2.stdout_data.find("second-service"), std::string::npos)
        << "stdout: " << res2.stdout_data;
}

#else // TC_HOST_PORT_FORWARDING

#include "testcontainers/Error.hpp"

// The stub HostPortForwarder must reject the run LOUDLY and EARLY: a clear
// DockerError naming the build option, thrown before any container is created
// (wire() runs ahead of create in the start orchestration). No daemon guard —
// nothing is ever asked of the daemon, so this refusal is verifiable anywhere
// the binary runs (unlike the sidecar suites above).
TEST(HostAccess, DisabledBuildThrowsClearError) {
    try {
        const Container c = GenericImage("alpine", "3.20")
                                .with_exposed_host_port(12345)
                                .with_cmd({"sleep", "120"})
                                .start();
        FAIL() << "expected start() to throw (this build has TC_HOST_PORT_FORWARDING=OFF)";
    } catch (const DockerError& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("TC_HOST_PORT_FORWARDING"), std::string::npos) << msg;
    }
}

#endif // TC_HOST_PORT_FORWARDING
