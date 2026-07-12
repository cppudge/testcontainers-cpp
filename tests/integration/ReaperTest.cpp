#include <gtest/gtest.h>

#include <chrono>
#include <exception>
#include <istream>
#include <string>
#include <thread>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <nlohmann/json.hpp>

#include "Reaper.hpp"
#include "testcontainers/Container.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon):
//   Reaper.AppliesLabelsAndStartsRyuk - a container started via GenericImage carries the managed-by and session-id labels and the session reaper came up.
//   Reaper.SameEndpointBootsNoSecondRyuk - the reaper registry keys daemons by endpoint URL: ensure_started with a fresh client for the already-served environment endpoint boots no second Ryuk.
//   Reaper.RyukReapsOnDisconnect - a dedicated Ryuk reaps a labelled (never-started) container once the control TCP connection is closed.

using namespace testcontainers;

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

// Read back the raw Config.Labels map of a container via a direct inspect call,
// since ContainerInspect does not surface labels.
nlohmann::json container_labels(DockerClient& client, const std::string& id) {
    const Response res = client.request("GET", "/containers/" + id + "/json");
    if (!res.ok()) {
        throw DockerError("inspect for labels failed: HTTP " + std::to_string(res.status_code));
    }
    const auto json = nlohmann::json::parse(res.body);
    if (json.contains("Config") && json["Config"].contains("Labels") &&
        json["Config"]["Labels"].is_object()) {
        return json["Config"]["Labels"];
    }
    return nlohmann::json::object();
}

} // namespace

// Requires a reachable Docker daemon; skipped if none is available.
class Reaper : public tcit::LinuxEngineTest {};

TEST_F(Reaper, AppliesLabelsAndStartsRyuk) {
    // Starting any container brings the global reaper up and tags the container.
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "30"}).start();

    DockerClient client = DockerClient::from_environment();
    const nlohmann::json labels = container_labels(client, c.id());

    ASSERT_TRUE(labels.contains("org.testcontainers.managed-by"));
    EXPECT_EQ(labels["org.testcontainers.managed-by"].get<std::string>(), "testcontainers");

    ASSERT_TRUE(labels.contains("org.testcontainers.session-id"));
    EXPECT_EQ(labels["org.testcontainers.session-id"].get<std::string>(), detail::session_id());

    // A Ryuk sidecar should be running. Scan the running container list and
    // confirm at least one has the testcontainers/ryuk image.
    const Response list = client.request("GET", "/containers/json");
    ASSERT_TRUE(list.ok()) << "container list failed: HTTP " << list.status_code;
    const auto arr = nlohmann::json::parse(list.body);
    bool found_ryuk = false;
    for (const auto& entry : arr) {
        const std::string image = entry.value("Image", std::string{});
        if (image.find("testcontainers/ryuk") != std::string::npos) {
            found_ryuk = true;
            break;
        }
    }
    EXPECT_TRUE(found_ryuk) << "no running testcontainers/ryuk container found";
}

TEST_F(Reaper, SameEndpointBootsNoSecondRyuk) {
    // Bring the environment daemon's reaper up (idempotent if an earlier test
    // already did), then count the running Ryuk sidecars. The exact-count
    // assertion below assumes nothing ELSE starts/stops Ryuks on this daemon
    // meanwhile — one integration binary at a time, gtest running serially.
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "30"}).start();

    DockerClient client = DockerClient::from_environment();
    const auto ryuk_count = [&client] {
        const Response list = client.request("GET", "/containers/json");
        if (!list.ok()) {
            throw DockerError("container list failed: HTTP " + std::to_string(list.status_code));
        }
        int count = 0;
        for (const auto& entry : nlohmann::json::parse(list.body)) {
            if (entry.value("Image", std::string{}).find("testcontainers/ryuk") !=
                std::string::npos) {
                ++count;
            }
        }
        return count;
    };

    const int before = ryuk_count();
    if (detail::ryuk_disabled()) {
        EXPECT_EQ(before, 0);
        GTEST_SKIP(); // nothing to dedup against
    }
    ASSERT_GE(before, 1);

    // A caller-supplied client for the SAME endpoint must reuse that entry.
    DockerClient another = DockerClient::from_environment();
    detail::Reaper::instance().ensure_started(another);
    EXPECT_EQ(ryuk_count(), before);
}

TEST_F(Reaper, RyukReapsOnDisconnect) {
    // Self-contained: drive a DEDICATED Ryuk (never touches the global reaper),
    // prove it reaps a labelled container once the control connection drops.
    DockerClient client = DockerClient::from_environment();

    // A unique throwaway label so this never collides with the process session.
    const std::string key = "org.testcontainers.cpp-reaper-test";
    const std::string value = detail::session_id() + "-reap"; // unique per run

    const detail::RyukEndpoint ryuk = detail::start_ryuk(client);
    std::string target_id;
    try {
        // Handshake with Ryuk's control port. The WHOLE exchange retries, not
        // just the connect: the daemon's userland docker-proxy accepts on the
        // published port before Ryuk itself listens, so the startup race
        // surfaces as "connection reset" on the write/ACK-read (bit on CI).
        asio::io_context io;
        tcp::socket socket(io);
        {
            const std::string line = detail::ryuk_filter_line(key, value);
            boost::system::error_code ec;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            bool handshaken = false;
            while (std::chrono::steady_clock::now() < deadline) {
                tcp::resolver resolver(io);
                const auto endpoints = resolver.resolve(ryuk.host, std::to_string(ryuk.port), ec);
                if (!ec) {
                    socket = tcp::socket(io);
                    asio::connect(socket, endpoints, ec);
                    if (!ec) {
                        asio::write(socket, asio::buffer(line), ec);
                    }
                    if (!ec) {
                        asio::streambuf buf;
                        asio::read_until(socket, buf, '\n', ec);
                        if (!ec) {
                            std::istream is(&buf);
                            std::string ack;
                            std::getline(is, ack);
                            ASSERT_NE(ack.find("ACK"), std::string::npos)
                                << "unexpected Ryuk reply: " << ack;
                            handshaken = true;
                            break;
                        }
                    }
                    boost::system::error_code ignore;
                    socket.close(ignore);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            ASSERT_TRUE(handshaken) << "Ryuk handshake failed: " << ec.message();
        }

        // Create (do NOT start) a container carrying the throwaway label.
        CreateContainerSpec spec;
        spec.image = "alpine:3.20";
        spec.cmd = {"sleep", "120"};
        spec.labels = {{key, value}};
        target_id = client.create_container(spec);
        ASSERT_FALSE(target_id.empty());
        // Sanity: it exists right now.
        ASSERT_NO_THROW(client.inspect_container(target_id));

        // Drop the control connection — this is what triggers reaping.
        socket.close();

        // Poll until the container is gone (Ryuk's 10s reconnection timeout +
        // processing). Allow generous headroom.
        bool reaped = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                client.inspect_container(target_id);
            } catch (const DockerError&) {
                reaped = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        EXPECT_TRUE(reaped) << "Ryuk did not reap the labelled container in time";
        if (reaped) {
            target_id.clear(); // already gone; nothing to clean up
        }
    } catch (...) {
        // Best-effort cleanup before rethrowing so we never leak the dedicated Ryuk.
        if (!target_id.empty()) {
            try {
                client.remove_container(target_id, true, true);
            } catch (...) {
                // Best-effort: still remove the dedicated Ryuk below.
            }
        }
        try {
            client.remove_container(ryuk.container_id, true, true);
        } catch (...) {
            // Best-effort: the original failure rethrown below is the cause.
        }
        throw;
    }

    // Clean up the dedicated Ryuk (and the target, if reaping somehow failed).
    if (!target_id.empty()) {
        try {
            client.remove_container(target_id, true, true);
        } catch (...) {
            // Best-effort: the target may already be gone.
        }
    }
    EXPECT_NO_THROW(client.remove_container(ryuk.container_id, true, true));
}
