#define _CRT_SECURE_NO_WARNINGS // std::getenv on MSVC

#include "Reaper.hpp"

#include "RandomHex.hpp"
#include "docker/Ports.hpp"
#include "docker/Transport.hpp" // throw_transport_error
#include "testcontainers/Error.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <istream>
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

bool env_truthy(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr) {
        return false;
    }
    const std::string s(v);
    return s == "1" || s == "true" || s == "TRUE" || s == "True";
}

/// Deadline-bounded run of the single pending async op on `io` (the transport
/// layer's run_pending pattern, see docker/Transport.cpp): on expiry invoke
/// `cancel`, drain the handler, and report asio::error::timed_out unless the
/// op genuinely completed while draining.
template <class Cancel>
void run_bounded(asio::io_context& io, std::chrono::milliseconds timeout, const bool& done,
                 boost::system::error_code& ec, Cancel cancel) {
    io.restart();
    io.run_for(timeout);
    if (done) {
        return;
    }
    cancel();
    io.run();
    if (!done || ec == asio::error::operation_aborted) {
        ec = asio::error::timed_out;
    }
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
    const ContainerInspect info = client.inspect_container(id);
    const auto host_port =
        docker::select_host_port(info.ports, kRyukPort, docker::HostPortFamily::Any);
    if (!host_port) {
        try {
            client.remove_container(id, /*force*/ true, /*remove_volumes*/ true);
        } catch (...) {
        }
        throw DockerError("Ryuk container " + id + " published no host port for " + kRyukPort);
    }

    RyukEndpoint ep;
    ep.container_id = id;
    ep.host = client.host().http_host(); // "localhost" for a named pipe / unix socket
    ep.port = *host_port;
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
    start_locked(client);
}

void Reaper::ensure_started(DockerClient& client) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || ryuk_disabled()) {
        return;
    }
    start_locked(client);
}

void Reaper::start_locked(DockerClient& client) {
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

    // Ryuk needs a moment to start listening, and retrying the TCP connect
    // alone is NOT enough: with the daemon's userland docker-proxy (the
    // default on plain Linux engines, e.g. CI runners) the proxy ACCEPTS on
    // the published port before Ryuk itself listens, and the failure shows up
    // one step later as "connection reset" on the filter write / ACK read.
    // So the WHOLE handshake — fresh connect, send filter, read ACK — retries
    // until the deadline; each leg is also individually bounded, so a
    // black-holed attempt cannot overshoot the budget.
    boost::system::error_code ec;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    const std::chrono::milliseconds leg_budget = std::chrono::seconds(5);
    const std::string line = ryuk_filter_line(kSessionIdLabel, session_id());
    const auto budget_left = [&deadline, &leg_budget] {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        return std::max(std::min(left, leg_budget), std::chrono::milliseconds(1));
    };

    std::string ack;
    bool handshaken = false;
    while (std::chrono::steady_clock::now() < deadline) {
        tcp::resolver resolver(impl->io);
        const auto endpoints = resolver.resolve(ep.host, std::to_string(ep.port), ec);
        if (ec) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        impl->socket = tcp::socket(impl->io);
        bool done = false;
        asio::async_connect(impl->socket, endpoints,
                            [&](const boost::system::error_code& op_ec, const tcp::endpoint&) {
                                done = true;
                                ec = op_ec;
                            });
        // Close-not-cancel on expiry: a cancelled multi-endpoint connect just
        // moves on to the next endpoint (see docker/Transport.cpp).
        run_bounded(impl->io, budget_left(), done, ec, [&] {
            boost::system::error_code ignore;
            impl->socket.close(ignore);
        });

        if (!ec) {
            done = false;
            asio::async_write(impl->socket, asio::buffer(line),
                              [&](const boost::system::error_code& op_ec, std::size_t) {
                                  done = true;
                                  ec = op_ec;
                              });
            run_bounded(impl->io, budget_left(), done, ec, [&] {
                boost::system::error_code ignore;
                impl->socket.cancel(ignore);
            });
        }

        if (!ec) {
            asio::streambuf buf;
            done = false;
            asio::async_read_until(impl->socket, buf, '\n',
                                   [&](const boost::system::error_code& op_ec, std::size_t) {
                                       done = true;
                                       ec = op_ec;
                                   });
            run_bounded(impl->io, budget_left(), done, ec, [&] {
                boost::system::error_code ignore;
                impl->socket.cancel(ignore);
            });
            if (!ec) {
                std::istream is(&buf);
                std::getline(is, ack);
                handshaken = true;
                break;
            }
        }

        // Any connection-level failure (refused, reset by the proxy, timed
        // out): drop the socket and try the whole exchange again.
        boost::system::error_code ignore;
        impl->socket.close(ignore);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!handshaken) {
        kill_ryuk();
        // The retry loop only exits early when its 20s budget is spent — type
        // on THAT, not on whichever ec the last attempt happened to leave
        // (a refused/reset final attempt is still an exhausted budget).
        throw TransportTimeoutError("Could not complete the Ryuk handshake at " + ep.host + ":" +
                                    std::to_string(ep.port) + " within the handshake budget"
                                    " (last error: " + ec.message() + ")");
    }

    // Ryuk replies "ACK" per accepted line (tolerate a trailing CR). Anything
    // else means the filter was NOT registered — fail loudly rather than run
    // without crash-safe reaping. (A live Ryuk answering garbage is a protocol
    // error, not a startup race — no retry.)
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
