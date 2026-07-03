#define _CRT_SECURE_NO_WARNINGS // std::getenv on MSVC

#include "Reaper.hpp"

#include "testcontainers/Error.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <istream>
#include <random>
#include <string>
#include <thread>

namespace testcontainers::detail {

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

/// The Ryuk image. Pinned to a known-good tag; see README roadmap notes.
constexpr const char* kRyukImage = "testcontainers/ryuk:0.11.0";
constexpr const char* kRyukPort = "8080/tcp";

constexpr const char* kManagedByLabel = "org.testcontainers.managed-by";
constexpr const char* kSessionIdLabel = "org.testcontainers.session-id";

/// Generate a random 32-hex-char id (128 bits of entropy).
std::string random_hex(std::size_t chars) {
    static constexpr char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    std::string out;
    out.reserve(chars);
    for (std::size_t i = 0; i < chars; ++i) {
        out.push_back(hex[dist(rd)]);
    }
    return out;
}

bool env_truthy(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr) {
        return false;
    }
    const std::string s(v);
    return s == "1" || s == "true" || s == "TRUE" || s == "True";
}

} // namespace

const std::string& session_id() {
    // Generated once, lives for the whole process.
    static const std::string id = random_hex(32);
    return id;
}

bool ryuk_disabled() { return env_truthy("TESTCONTAINERS_RYUK_DISABLED"); }

std::vector<std::pair<std::string, std::string>> testcontainers_labels() {
    std::vector<std::pair<std::string, std::string>> labels;
    labels.emplace_back(kManagedByLabel, "testcontainers");
    // The session-id label only matters when there is a Ryuk reaper watching for
    // it; with Ryuk disabled we still mark resources as managed-by but skip it.
    if (!ryuk_disabled()) {
        labels.emplace_back(kSessionIdLabel, session_id());
    }
    return labels;
}

std::string ryuk_filter_line(const std::string& key, const std::string& value) {
    return "label=" + key + "=" + value + "\n";
}

RyukEndpoint start_ryuk(DockerClient& client, bool auto_remove) {
    CreateContainerSpec spec;
    spec.image = kRyukImage;
    spec.exposed_ports = {kRyukPort};
    spec.publish_all_ports = true;
    // Ryuk talks to the daemon over the Linux docker socket. Even on Windows
    // Docker Desktop the source is this in-VM path (NOT the named pipe).
    spec.mounts = {Mount::bind("/var/run/docker.sock", "/var/run/docker.sock")};
    // Ryuk reaps when all its connections drop; give the reconnect window a
    // small, explicit value so an abrupt process death is cleaned up promptly.
    spec.env = {"RYUK_RECONNECTION_TIMEOUT=10s"};
    // When requested, let Docker delete the Ryuk container once it exits (after
    // reaping) so it doesn't leave a stopped shell behind across runs.
    spec.auto_remove = auto_remove;
    // Ryuk must NOT carry the session-id label (it would otherwise reap itself);
    // tag it managed-by only so it is still recognizable as ours.
    spec.labels = {{kManagedByLabel, "testcontainers"}};

    const std::string id = client.create_container(spec);
    client.start_container(id);

    // Resolve the published host port (prefer the IPv4 binding).
    std::uint16_t host_port = 0;
    {
        const ContainerInspect info = client.inspect_container(id);
        const auto it = info.ports.find(kRyukPort);
        if (it == info.ports.end() || it->second.empty()) {
            try {
                client.remove_container(id, /*force*/ true, /*remove_volumes*/ true);
            } catch (...) {
            }
            throw DockerError("Ryuk container " + id + " published no host port for " +
                              kRyukPort);
        }
        host_port = it->second.front().host_port;
        for (const PortBinding& binding : it->second) {
            if (binding.host_ip.find(':') == std::string::npos) { // IPv4 / empty
                host_port = binding.host_port;
                break;
            }
        }
    }

    RyukEndpoint ep;
    ep.container_id = id;
    ep.host = client.host().http_host(); // "localhost" for a named pipe / unix socket
    ep.port = host_port;
    return ep;
}

/// Holds the io_context + persistent control socket for the process lifetime.
struct Reaper::Impl {
    asio::io_context io;
    tcp::socket socket{io};
    std::string container_id;
};

Reaper::Reaper() = default;
Reaper::~Reaper() = default;

Reaper& Reaper::instance() {
    static Reaper reaper;
    return reaper;
}

void Reaper::ensure_started() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || ryuk_disabled()) {
        return;
    }

    DockerClient client = DockerClient::from_environment();

    // No Linux Ryuk image can run on a Windows-containers engine, so we skip the
    // reaper entirely there (matching testcontainers-dotnet). The managed-by /
    // session-id labels still get applied to user containers — harmless, just
    // unused. Consequence: there is NO crash-safe reaping on the Windows engine;
    // cleanup relies on each container's RAII removal (and AutoRemove on exit).
    if (client.is_windows_engine()) {
        started_ = true;
        return;
    }

    const RyukEndpoint ep = start_ryuk(client, /*auto_remove*/ true);

    // Best-effort removal of the Ryuk container on any startup failure below.
    const auto kill_ryuk = [&client, &ep]() noexcept {
        try {
            client.remove_container(ep.container_id, /*force*/ true, /*remove_volumes*/ true);
        } catch (...) {
        }
    };

    auto impl = std::make_unique<Impl>();
    impl->container_id = ep.container_id;

    // Ryuk needs a moment to start listening; connect-retry for a few seconds.
    boost::system::error_code ec;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool connected = false;
    while (std::chrono::steady_clock::now() < deadline) {
        tcp::resolver resolver(impl->io);
        const auto endpoints = resolver.resolve(ep.host, std::to_string(ep.port), ec);
        if (!ec) {
            impl->socket = tcp::socket(impl->io);
            asio::connect(impl->socket, endpoints, ec);
            if (!ec) {
                connected = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!connected) {
        kill_ryuk();
        throw DockerError("Could not connect to Ryuk control port at " + ep.host + ":" +
                          std::to_string(ep.port) + ": " + ec.message());
    }

    // Register the session filter and wait for the ACK so we know Ryuk accepted
    // it before we start creating labelled resources.
    const std::string line = ryuk_filter_line(kSessionIdLabel, session_id());
    asio::write(impl->socket, asio::buffer(line), ec);
    if (ec) {
        kill_ryuk();
        throw DockerError("Failed to send filter to Ryuk: " + ec.message());
    }

    asio::streambuf buf;
    asio::read_until(impl->socket, buf, '\n', ec);
    if (ec) {
        kill_ryuk();
        throw DockerError("Failed to read ACK from Ryuk: " + ec.message());
    }
    std::istream is(&buf);
    std::string ack;
    std::getline(is, ack);
    // Ryuk replies "ACK" per accepted line (tolerate a trailing CR). Anything
    // else means the filter was NOT registered — fail loudly rather than run
    // without crash-safe reaping.
    if (ack.rfind("ACK", 0) != 0) {
        kill_ryuk();
        throw DockerError("Ryuk did not acknowledge the session filter (got '" + ack + "')");
    }

    // Hold the socket open for the rest of the process — when the process dies,
    // the OS closes it and Ryuk reaps everything tagged with our session id.
    impl_ = std::move(impl);
    started_ = true;
}

} // namespace testcontainers::detail
