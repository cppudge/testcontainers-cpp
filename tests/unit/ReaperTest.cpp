#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "CannedHttpServer.hpp"
#include "Reaper.hpp"
#include "TestEnv.hpp"
#include "TestSupport.hpp"
#include "testcontainers/docker/DockerClient.hpp"

// Tests in this file:
//   Reaper.SessionIdNonEmptyAndStable - session_id() returns a non-empty value that is identical across calls.
//   Reaper.RyukFilterLine - ryuk_filter_line builds the "label=<key>=<value>\n" line Ryuk expects.
//   Reaper.LabelsContainManagedBy - testcontainers_labels() always carries the managed-by label.
//   Reaper.LabelsContainSessionIdWhenEnabled - testcontainers_labels() carries the session-id label (== session_id()) unless Ryuk is disabled.
//   Reaper.PerDaemonMapBootsDedupsAndSkips - against canned daemons + fake Ryuks: each daemon boots its own Ryuk exactly once (session filter ACKed there), repeat ensure_started calls add no traffic, register_filter reaches the ENVIRONMENT daemon's Ryuk (once, dedup across repeats, visible via registered_filters), and a Windows-mode daemon is skipped entirely (no create; register_filter a no-op).

using namespace testcontainers::detail;

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

/// A minimal stand-in for Ryuk's control port: accepts a connection, reads
/// newline-terminated filter lines, ACKs each, and records them. Reads poll a
/// non-blocking socket so the destructor's stop flag can end a connection the
/// CLIENT never closes (the reaper holds its control socket for the process
/// lifetime) without cross-thread Asio calls.
class FakeRyuk {
public:
    FakeRyuk()
        : acceptor_(io_, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
          port_(acceptor_.local_endpoint().port()), thread_([this] { loop(); }) {}

    ~FakeRyuk() {
        stop_ = true;
        // Unblock a parked accept with a throwaway connection instead of
        // closing the acceptor under the server thread (CannedHttpServer's
        // teardown idiom).
        try {
            asio::io_context poke_io;
            tcp::socket poke(poke_io);
            boost::system::error_code ignore;
            poke.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port_), ignore);
        } catch (...) {
            // Best-effort: the join below is what matters.
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        boost::system::error_code ignore;
        acceptor_.close(ignore);
    }

    FakeRyuk(const FakeRyuk&) = delete;
    FakeRyuk& operator=(const FakeRyuk&) = delete;

    std::uint16_t port() const { return port_; }

    /// The filter lines received so far (newline stripped), in order.
    std::vector<std::string> lines() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

private:
    void loop() {
        while (!stop_) {
            tcp::socket socket(io_);
            boost::system::error_code ec;
            acceptor_.accept(socket, ec);
            if (ec || stop_) {
                return;
            }
            serve(socket);
        }
    }

    void serve(tcp::socket& socket) {
        boost::system::error_code ec;
        socket.non_blocking(true, ec);
        std::string pending;
        std::array<char, 512> buf{};
        while (!stop_) {
            const std::size_t n = socket.read_some(asio::buffer(buf), ec);
            if (ec == asio::error::would_block) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (ec) {
                return; // peer closed/reset: back to accept
            }
            pending.append(buf.data(), n);
            for (std::size_t nl = pending.find('\n'); nl != std::string::npos;
                 nl = pending.find('\n')) {
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    lines_.push_back(pending.substr(0, nl));
                }
                pending.erase(0, nl + 1);
                if (!write_all(socket, "ACK\n")) {
                    return;
                }
            }
        }
    }

    /// write_some until `text` fully left (a 4-byte loopback write leaves
    /// would_block within a poll tick or two).
    bool write_all(tcp::socket& socket, const std::string& text) {
        boost::system::error_code ec;
        std::size_t written = 0;
        while (written < text.size() && !stop_) {
            const std::size_t n =
                socket.write_some(asio::buffer(text.data() + written, text.size() - written), ec);
            if (ec == asio::error::would_block) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (ec) {
                return false;
            }
            written += n;
        }
        return written == text.size();
    }

    asio::io_context io_;
    tcp::acceptor acceptor_;
    std::uint16_t port_ = 0;
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
    std::atomic<bool> stop_{false};
    std::thread thread_; ///< declared last: starts after the members it uses
};

std::string version_response(const std::string& os) {
    return tcunit::http_response(200, "OK", R"({"Os":")" + os + R"(","ApiVersion":"1.43"})");
}

/// The inspect start_ryuk consumes: running, 8080/tcp published to `host_port`.
std::string inspect_ryuk(std::uint16_t host_port) {
    return tcunit::http_response(
        200, "OK",
        R"({"Id":"ryuk-under-test","Name":"/ryuk","State":{"Status":"running","Running":true},)"
        R"("NetworkSettings":{"Ports":{"8080/tcp":[{"HostIp":"0.0.0.0","HostPort":")" +
            std::to_string(host_port) + R"("}]}}})");
}

std::string no_content() { return tcunit::http_response_no_body(204, "No Content"); }

/// The canned boot sequence one daemon serves: negotiation ping, the engine-os
/// probe, then Ryuk's create/start/inspect.
std::vector<std::string> linux_boot_script(std::uint16_t ryuk_port) {
    return {tcunit::ping_ok(), version_response("linux"), tcunit::created("ryuk-under-test"),
            no_content(), inspect_ryuk(ryuk_port)};
}

} // namespace

TEST(Reaper, SessionIdNonEmptyAndStable) {
    const std::string& a = session_id();
    const std::string& b = session_id();
    EXPECT_FALSE(a.empty());
    EXPECT_EQ(a, b);
    // Same backing object every call (process-lifetime singleton).
    EXPECT_EQ(&a, &b);
}

TEST(Reaper, RyukFilterLine) {
    EXPECT_EQ(ryuk_filter_line("k", "v"), "label=k=v\n");
    EXPECT_EQ(ryuk_filter_line("org.testcontainers.session-id", "abc123"),
              "label=org.testcontainers.session-id=abc123\n");
}

TEST(Reaper, LabelsContainManagedBy) {
    const auto labels = testcontainers_labels();
    const bool has_managed_by = std::any_of(labels.begin(), labels.end(), [](const auto& kv) {
        return kv.first == "org.testcontainers.managed-by" && kv.second == "testcontainers";
    });
    EXPECT_TRUE(has_managed_by);
}

TEST(Reaper, LabelsContainSessionIdWhenEnabled) {
    const auto labels = testcontainers_labels();
    const auto it = std::find_if(labels.begin(), labels.end(), [](const auto& kv) {
        return kv.first == "org.testcontainers.session-id";
    });
    if (ryuk_disabled()) {
        // With the reaper off there is no session label to apply.
        EXPECT_EQ(it, labels.end());
    } else {
        ASSERT_NE(it, labels.end());
        EXPECT_EQ(it->second, session_id());
    }
}

TEST(Reaper, PerDaemonMapBootsDedupsAndSkips) {
    if (ryuk_disabled()) {
        GTEST_SKIP(); // the boot path under test is disabled in this environment
    }

    // ONE test covers the whole map behavior so every canned endpoint is alive
    // (and therefore port-distinct) at once: the process-global registry keys
    // daemons by endpoint URL, and a recycled port would alias a dead entry.
    // (That protects a single pass only — under --gtest_repeat a recycled
    // ephemeral port could still alias an entry from a previous iteration.)
    FakeRyuk ryuk1;
    FakeRyuk ryuk2;
    tcunit::CannedHttpServer daemon1(linux_boot_script(ryuk1.port()));
    tcunit::CannedHttpServer daemon2(linux_boot_script(ryuk2.port()));
    tcunit::CannedHttpServer daemon3({tcunit::ping_ok(), version_response("windows")});

    testcontainers::DockerClient c1{daemon1.host()};
    testcontainers::DockerClient c2{daemon2.host()};
    testcontainers::DockerClient c3{daemon3.host()};
    Reaper& reaper = Reaper::instance();

    // Each daemon boots its own Ryuk; repeats (same client or a fresh client
    // for the same endpoint) add no traffic.
    reaper.ensure_started(c1);
    reaper.ensure_started(c2);
    reaper.ensure_started(c1);
    testcontainers::DockerClient c1_again{daemon1.host()};
    reaper.ensure_started(c1_again);

    ASSERT_EQ(ryuk1.lines().size(), 1u);
    EXPECT_NE(ryuk1.lines()[0].find("label=org.testcontainers.session-id="), std::string::npos)
        << ryuk1.lines()[0];
    ASSERT_EQ(ryuk2.lines().size(), 1u);
    EXPECT_NE(ryuk2.lines()[0].find("label=org.testcontainers.session-id="), std::string::npos)
        << ryuk2.lines()[0];

    // Exactly one boot each: ping, version, create, start, inspect.
    ASSERT_EQ(daemon1.requests().size(), 5u);
    EXPECT_EQ(daemon2.requests().size(), 5u);
    EXPECT_NE(daemon1.requests()[2].find("testcontainers/ryuk"), std::string::npos)
        << daemon1.requests()[2];

    // register_filter targets the ENVIRONMENT daemon (daemon1 via DOCKER_HOST):
    // the line reaches ITS Ryuk, once, and shows up in registered_filters().
    {
        const tctest::ScopedEnv docker_host("DOCKER_HOST", daemon1.host().to_string());
        reaper.register_filter("com.docker.compose.project", "tcunitproj");
        reaper.register_filter("com.docker.compose.project", "tcunitproj"); // dedup
        ASSERT_EQ(ryuk1.lines().size(), 2u);
        EXPECT_EQ(ryuk1.lines()[1] + "\n",
                  ryuk_filter_line("com.docker.compose.project", "tcunitproj"));
        EXPECT_EQ(ryuk2.lines().size(), 1u); // the OTHER daemon's Ryuk saw nothing
        const std::vector<std::string> filters = reaper.registered_filters();
        EXPECT_EQ(std::count(filters.begin(), filters.end(),
                             ryuk_filter_line("com.docker.compose.project", "tcunitproj")),
                  1);
        EXPECT_EQ(daemon1.requests().size(), 5u); // no daemon traffic for a filter
    }

    // A Windows-containers daemon is skipped: probe only (ping + version), no
    // create; register_filter against it is a no-op with nothing registered.
    reaper.ensure_started(c3);
    EXPECT_EQ(daemon3.requests().size(), 2u);
    {
        const tctest::ScopedEnv docker_host("DOCKER_HOST", daemon3.host().to_string());
        reaper.register_filter("com.docker.compose.project", "tcunitproj");
        EXPECT_TRUE(reaper.registered_filters().empty());
        EXPECT_EQ(daemon3.requests().size(), 2u);
    }
}
