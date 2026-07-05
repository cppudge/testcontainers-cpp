#include "HostPortForwarding.hpp"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <libssh2.h>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h> // OPENSSL_thread_stop (pump-thread state cleanup)

#ifndef _WIN32
#include <sys/select.h> // select() for the tunnel pump (winsock covers Windows)
#endif

#include "RandomHex.hpp"
#include "Runner.hpp"
#include "testcontainers/Container.hpp"
#include "testcontainers/ContainerRequest.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"

namespace testcontainers::detail {

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

/// The sidecar image every Testcontainers implementation shares: alpine +
/// openssh; its own entrypoint+command set ${USERNAME} (root) to ${PASSWORD}
/// and run sshd in the foreground with GatewayPorts=yes.
constexpr const char* kSshdImage = "testcontainers/sshd";
constexpr const char* kSshdTag = "1.3.0";

/// Per-direction buffer cap per forwarded connection. When a buffer is full
/// the reading side is paused until the writing side drains it (backpressure
/// instead of unbounded growth).
constexpr std::size_t kConnBufferCap = std::size_t{256} * 1024;

/// The last libssh2 error on `session`, as "message (rc N)".
std::string ssh_error(LIBSSH2_SESSION* session) {
    char* msg = nullptr;
    const int rc = libssh2_session_last_error(session, &msg, nullptr, 0);
    std::string text = (msg && *msg) ? msg : "unknown libssh2 error";
    return text + " (rc " + std::to_string(rc) + ")";
}

/// True when a libssh2 return code means the SSH SESSION is unusable (socket
/// dead or protocol violation), as opposed to a single-channel failure.
bool is_session_fatal(int rc) {
    return rc == LIBSSH2_ERROR_SOCKET_SEND || rc == LIBSSH2_ERROR_SOCKET_RECV ||
           rc == LIBSSH2_ERROR_SOCKET_DISCONNECT || rc == LIBSSH2_ERROR_PROTO ||
           rc == LIBSSH2_ERROR_BAD_SOCKET;
}

/// Serializes libssh2_init/libssh2_exit (refcounted but not thread-safe).
std::mutex& global_init_mu() {
    static std::mutex mu;
    return mu;
}

/// An SSH connection to the sshd sidecar carrying the remote port forwards.
///
/// libssh2 multiplexes every forward over ONE session on ONE socket, and a
/// session is not thread-safe — so all post-handshake libssh2 calls happen
/// under `mu_`: the pump thread takes it per iteration (releasing it while
/// parked in select()), and `add_forward` takes it to register a listener.
class SshTunnel {
public:
    /// Connects, handshakes, and authenticates (blocking); then switches the
    /// session to non-blocking and starts the pump thread. Throws DockerError
    /// on any failure.
    SshTunnel(const std::string& host, std::uint16_t port, const std::string& user,
              const std::string& password)
        : ssh_sock_(io_) {
        // Paired with libssh2_exit() in the destructor (and the ctor failure
        // path): libssh2 refcounts init/exit, so the last tunnel releases the
        // crypto backend's global allocations — a fire-once init would hold
        // them until process exit and read as a leak under LeakSanitizer.
        // The pair is refcounted but NOT thread-safe, hence the mutex.
        {
            const std::lock_guard<std::mutex> lk(global_init_mu());
            libssh2_init(0);
        }

        try {
            tcp::resolver resolver(io_);
            asio::connect(ssh_sock_, resolver.resolve(host, std::to_string(port)));
            ssh_sock_.set_option(tcp::no_delay(true));
            // The NATIVE socket must be non-blocking (a synchronous
            // asio::connect leaves it blocking): a non-blocking libssh2
            // SESSION only returns EAGAIN when recv() itself would block —
            // on a blocking socket the pump would park inside recv() while
            // HOLDING mu_, deadlocking add_forward/dead() and the process
            // exit join. The blocking-mode handshake below still works:
            // libssh2 waits out EAGAIN internally in that mode.
            ssh_sock_.non_blocking(true);

            session_ = libssh2_session_init();
            if (!session_) {
                throw DockerError("libssh2_session_init failed");
            }
            libssh2_session_set_blocking(session_, 1);
            if (libssh2_session_handshake(session_, ssh_sock_.native_handle()) != 0) {
                throw DockerError("SSH handshake with the host-access sidecar failed: " +
                                  ssh_error(session_));
            }
            // The sidecar is a container we just created with a random
            // password — there is no meaningful host key to verify.
            if (libssh2_userauth_password(session_, user.c_str(), password.c_str()) != 0) {
                throw DockerError("SSH authentication with the host-access sidecar failed: " +
                                  ssh_error(session_));
            }
            // Long test suites can idle for minutes between containers; keep
            // the connection (and any NAT/proxy on the way) alive. want_reply
            // MUST stay 0: a keepalive answered with SSH_MSG_REQUEST_FAILURE
            // gets misattributed by libssh2 to a concurrent tcpip-forward
            // request, "denying" a perfectly valid forward.
            libssh2_keepalive_config(session_, 0, 30);
            libssh2_session_set_blocking(session_, 0);
        } catch (...) {
            if (session_) {
                libssh2_session_free(session_);
                session_ = nullptr;
            }
            const std::lock_guard<std::mutex> lk(global_init_mu());
            libssh2_exit();
            throw;
        }

        // OPENSSL_thread_stop: libssh2's crypto backend (OpenSSL 3) lazily
        // allocates per-thread DRBG + error state on this thread at the first
        // keepalive; without the explicit stop that state outlives the thread
        // (the auto TLS-destructor cleanup does not fire here) and reads as a
        // leak under LeakSanitizer.
        pump_ = std::thread([this] {
            loop();
            OPENSSL_thread_stop();
        });
    }

    SshTunnel(const SshTunnel&) = delete;
    SshTunnel& operator=(const SshTunnel&) = delete;

    ~SshTunnel() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        if (pump_.joinable()) {
            pump_.join();
        }
        // The pump thread is gone: tear everything down single-threaded, in
        // blocking mode so the frees complete instead of returning EAGAIN.
        libssh2_session_set_blocking(session_, 1);
        for (Conn& c : conns_) {
            libssh2_channel_free(c.ch);
        }
        for (Listener& l : listeners_) {
            libssh2_channel_forward_cancel(l.listener);
        }
        libssh2_session_disconnect(session_, "testcontainers shutdown");
        libssh2_session_free(session_);
        boost::system::error_code ignored;
        ssh_sock_.close(ignored);
        // Release the refcounted global init taken in the constructor.
        const std::lock_guard<std::mutex> lk(global_init_mu());
        libssh2_exit();
    }

    /// Ask sshd to listen on `port` (all interfaces — GatewayPorts=yes) and
    /// forward every connection back to us. Throws DockerError on failure.
    void add_forward(std::uint16_t port) {
        std::lock_guard<std::mutex> lk(mu_);
        if (dead_) {
            throw DockerError("the SSH tunnel to the host-access sidecar is down: " + why_dead_);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            int bound = 0;
            LIBSSH2_LISTENER* listener =
                libssh2_channel_forward_listen_ex(session_, nullptr, port, &bound, 16);
            if (listener) {
                listeners_.push_back(Listener{listener, port});
                return;
            }
            const int rc = libssh2_session_last_errno(session_);
            if (rc != LIBSSH2_ERROR_EAGAIN) {
                throw DockerError("requesting the remote forward for host port " +
                                  std::to_string(port) + " failed: " + ssh_error(session_));
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw DockerError("timed out requesting the remote forward for host port " +
                                  std::to_string(port));
            }
            wait_ssh_socket(std::chrono::milliseconds(50));
        }
    }

    bool dead() {
        std::lock_guard<std::mutex> lk(mu_);
        return dead_;
    }

private:
    struct Listener {
        LIBSSH2_LISTENER* listener = nullptr;
        std::uint16_t port = 0; ///< forwarded port == local service port
    };

    /// One forwarded connection: an SSH channel (the in-container client) tied
    /// to a local socket (the host service), with per-direction staging
    /// buffers so each side can progress independently.
    struct Conn {
        explicit Conn(asio::io_context& io) : sock(io) {}
        LIBSSH2_CHANNEL* ch = nullptr;
        tcp::socket sock;
        std::string to_sock;   ///< channel -> local service, pending bytes
        std::string to_ch;     ///< local service -> channel, pending bytes
        bool ch_eof = false;   ///< remote client half-closed
        bool sock_eof = false; ///< local service half-closed
        bool sock_send_shut = false;
        bool ch_eof_sent = false;
        bool done = false; ///< both directions finished; free the channel
    };

    /// select() on the SSH socket (used to wait out EAGAIN while holding mu_).
    void wait_ssh_socket(std::chrono::milliseconds timeout) {
        fd_set rfds;
        fd_set wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        const libssh2_socket_t fd = ssh_sock_.native_handle();
        FD_SET(fd, &rfds);
        FD_SET(fd, &wfds);
        timeval tv{};
        tv.tv_sec = static_cast<long>(timeout.count() / 1000);
        tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
        select(static_cast<int>(fd) + 1, &rfds, &wfds, nullptr, &tv);
    }

    /// Mark the whole tunnel dead (SSH transport failure): close every
    /// forwarded connection now; the session itself is torn down by ~SshTunnel.
    void fail_locked(std::string why) {
        dead_ = true;
        why_dead_ = std::move(why);
        for (Conn& c : conns_) {
            boost::system::error_code ignored;
            c.sock.close(ignored);
        }
    }

    void loop() {
        for (;;) {
            libssh2_socket_t ssh_fd = 0;
            std::vector<std::pair<libssh2_socket_t, bool>> conn_fds; // fd, want_write
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (stop_ || dead_) {
                    return;
                }
                pump_locked();
                if (stop_ || dead_) {
                    return;
                }
                ssh_fd = ssh_sock_.native_handle();
                conn_fds.reserve(conns_.size());
                for (Conn& c : conns_) {
                    if (!c.done && c.sock.is_open()) {
                        conn_fds.emplace_back(c.sock.native_handle(), !c.to_sock.empty());
                    }
                }
            }

            // Park until something can progress, or 100ms — the timeout also
            // paces keepalives and retries of EAGAIN'd frees/EOFs.
            fd_set rfds;
            fd_set wfds;
            FD_ZERO(&rfds);
            FD_ZERO(&wfds);
            FD_SET(ssh_fd, &rfds);
            int maxfd = static_cast<int>(ssh_fd);
            for (const auto& [fd, want_write] : conn_fds) {
                FD_SET(fd, &rfds);
                if (want_write) {
                    FD_SET(fd, &wfds);
                }
                maxfd = std::max(maxfd, static_cast<int>(fd));
            }
            timeval tv{};
            tv.tv_usec = 100L * 1000;
            select(maxfd + 1, &rfds, &wfds, nullptr, &tv);
        }
    }

    /// One non-blocking pass over the whole session: keepalive, accept newly
    /// forwarded connections, and pump bytes both ways on every connection.
    void pump_locked() {
        libssh2_keepalive_send(session_, nullptr); // best-effort; errors surface below

        // Accept connections that arrived at sshd on any forwarded port.
        for (const Listener& l : listeners_) {
            for (;;) {
                LIBSSH2_CHANNEL* ch = libssh2_channel_forward_accept(l.listener);
                if (!ch) {
                    const int rc = libssh2_session_last_errno(session_);
                    if (rc != LIBSSH2_ERROR_EAGAIN && is_session_fatal(rc)) {
                        fail_locked("accepting a forwarded connection failed: " +
                                    ssh_error(session_));
                        return;
                    }
                    break;
                }
                conns_.emplace_back(io_);
                Conn& c = conns_.back();
                c.ch = ch;
                boost::system::error_code ec;
                c.sock.connect(tcp::endpoint(asio::ip::address_v4::loopback(), l.port), ec);
                if (ec) {
                    // No local service behind the forward: drop the connection
                    // (the in-container client sees a close, like a refused
                    // connect).
                    c.done = true;
                } else {
                    c.sock.non_blocking(true, ec);
                }
            }
        }

        for (Conn& c : conns_) {
            if (!c.done) {
                pump_conn_locked(c);
            }
            if (dead_) {
                return;
            }
        }

        // Free channels of finished connections (channel_free itself can
        // EAGAIN — those stay and are retried next pass).
        for (auto it = conns_.begin(); it != conns_.end();) {
            if (it->done && libssh2_channel_free(it->ch) != LIBSSH2_ERROR_EAGAIN) {
                boost::system::error_code ignored;
                it->sock.close(ignored);
                it = conns_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void pump_conn_locked(Conn& c) {
        char buf[16 * 1024];

        // channel -> to_sock (paused while the buffer is at its cap)
        while (!c.ch_eof && c.to_sock.size() < kConnBufferCap) {
            const auto n = libssh2_channel_read(c.ch, buf, sizeof(buf));
            if (n > 0) {
                c.to_sock.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) {
                if (libssh2_channel_eof(c.ch)) {
                    c.ch_eof = true;
                }
                break;
            }
            if (n == LIBSSH2_ERROR_EAGAIN) {
                break;
            }
            if (is_session_fatal(static_cast<int>(n))) {
                fail_locked("reading a forwarded connection failed: " + ssh_error(session_));
            }
            return conn_error_locked(c);
        }

        boost::system::error_code ec;

        // to_sock -> local service
        while (!c.to_sock.empty()) {
            const std::size_t n = c.sock.write_some(asio::buffer(c.to_sock), ec);
            if (ec) {
                if (ec == asio::error::would_block) {
                    break;
                }
                return conn_error_locked(c);
            }
            c.to_sock.erase(0, n);
        }
        if (c.ch_eof && c.to_sock.empty() && !c.sock_send_shut) {
            c.sock.shutdown(tcp::socket::shutdown_send, ec);
            c.sock_send_shut = true;
        }

        // local service -> to_ch (paused while the buffer is at its cap)
        while (!c.sock_eof && c.to_ch.size() < kConnBufferCap) {
            const std::size_t n = c.sock.read_some(asio::buffer(buf), ec);
            if (ec) {
                if (ec == asio::error::would_block) {
                    break;
                }
                if (ec == asio::error::eof) {
                    c.sock_eof = true;
                    break;
                }
                return conn_error_locked(c);
            }
            c.to_ch.append(buf, n);
        }

        // to_ch -> channel
        while (!c.to_ch.empty()) {
            const auto n = libssh2_channel_write(c.ch, c.to_ch.data(), c.to_ch.size());
            if (n == LIBSSH2_ERROR_EAGAIN) {
                break;
            }
            if (n < 0) {
                if (is_session_fatal(static_cast<int>(n))) {
                    fail_locked("writing a forwarded connection failed: " + ssh_error(session_));
                }
                return conn_error_locked(c);
            }
            c.to_ch.erase(0, static_cast<std::size_t>(n));
        }
        if (c.sock_eof && c.to_ch.empty() && !c.ch_eof_sent) {
            if (libssh2_channel_send_eof(c.ch) == 0) {
                c.ch_eof_sent = true; // EAGAIN: flag stays false, retried next pass
            }
        }

        if (c.ch_eof && c.sock_eof && c.to_sock.empty() && c.to_ch.empty()) {
            c.done = true;
        }
    }

    /// A single connection failed (its socket errored or its channel broke):
    /// close it without taking the tunnel down.
    void conn_error_locked(Conn& c) {
        boost::system::error_code ignored;
        c.sock.close(ignored);
        c.done = true;
    }

    asio::io_context io_;
    tcp::socket ssh_sock_;
    LIBSSH2_SESSION* session_ = nullptr;

    std::mutex mu_; ///< guards the session, listeners_, conns_, stop_/dead_
    std::vector<Listener> listeners_;
    std::vector<Conn> conns_;
    bool stop_ = false;
    bool dead_ = false;
    std::string why_dead_;

    std::thread pump_;
};

/// The sidecar's IP address as seen on `network` (a network NAME as it appears
/// in `NetworkSettings.Networks`). With `fallback_first`, an absent entry
/// falls back to the first network the container is on (a daemon with a
/// renamed default bridge). Throws DockerError when no address can be found.
std::string ip_on_network(DockerClient& client, const std::string& container_id,
                          const std::string& network, bool fallback_first) {
    const auto body = nlohmann::json::parse(client.inspect_container_raw(container_id));
    const auto& nets = body.at("NetworkSettings").at("Networks");
    std::string ip;
    if (nets.contains(network)) {
        ip = nets.at(network).value("IPAddress", "");
    } else if (fallback_first && !nets.empty()) {
        ip = nets.begin().value().value("IPAddress", "");
    }
    if (ip.empty()) {
        throw DockerError("the host-access sidecar has no IP address on network '" + network + "'");
    }
    return ip;
}

/// Resolve a network reference (name or id, as accepted by the Docker API)
/// to its NAME — the key `NetworkSettings.Networks` uses.
std::string resolve_network_name(DockerClient& client, const std::string& ref) {
    Response res = client.request("GET", "/networks/" + ref);
    if (!res.ok()) {
        throw DockerError("inspecting network '" + ref + "' failed: HTTP " +
                              std::to_string(res.status_code) + " " + res.body,
                          res.status_code, ref);
    }
    return nlohmann::json::parse(res.body).at("Name").get<std::string>();
}

} // namespace

struct HostPortForwarder::State {
    // Declaration order is teardown-critical: `tunnel` (declared later) is
    // destroyed FIRST — it joins the pump thread — and only then is the
    // sidecar container removed out from under it.
    std::optional<Container> sidecar;
    std::unique_ptr<SshTunnel> tunnel;
    std::string sidecar_id;
    std::string bridge_ip; ///< sidecar's IP on the default bridge
    std::set<std::uint16_t> forwarded;
    std::set<std::string> connected_networks; ///< user networks the sidecar joined
};

HostPortForwarder& HostPortForwarder::instance() {
    // Initialize OpenSSL BEFORE the static below is constructed. Exit-time
    // handlers run newest-first, so this makes OPENSSL_cleanup run AFTER the
    // forwarder's destructor. Left to lazy init (the first SSH handshake,
    // inside the already-constructed forwarder), the order inverts: cleanup
    // tears OpenSSL down first, and the tunnel teardown that follows
    // re-allocates crypto state nothing will ever free.
    OPENSSL_init_crypto(0, nullptr);
    static HostPortForwarder forwarder;
    return forwarder;
}

HostPortForwarder::~HostPortForwarder() = default;

void HostPortForwarder::release_network(DockerClient& client, const std::string& network) noexcept {
    try {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!state_) {
            return;
        }
        // wire() records networks by NAME; the caller may pass a name or id.
        std::string name = network;
        if (state_->connected_networks.count(name) == 0) {
            name = resolve_network_name(client, network);
        }
        if (state_->connected_networks.erase(name) == 0) {
            return;
        }
        client.disconnect_network(network, state_->sidecar_id);
    } catch (...) {
        // Best-effort teardown aid; the network removal that follows reports
        // its own failure if this did not help.
    }
}

void HostPortForwarder::wire(DockerClient& client, CreateContainerSpec& spec,
                             const std::vector<std::uint16_t>& ports) {
    std::lock_guard<std::mutex> lk(mutex_);

    if (client.is_windows_engine()) {
        throw DockerError("with_exposed_host_port requires a Linux-containers daemon: the "
                          "host-access sidecar (testcontainers/sshd) is a Linux image");
    }
    if (spec.network) {
        const std::string& mode = *spec.network;
        if (mode == "host" || mode == "none" || mode.rfind("container:", 0) == 0) {
            throw DockerError("with_exposed_host_port does not support network mode '" + mode +
                              "'");
        }
    }

    // A dead tunnel (daemon restarted, sidecar killed, …) is not recoverable
    // in place — drop the whole state and start a fresh sidecar.
    if (state_ && state_->tunnel->dead()) {
        state_.reset();
    }
    if (!state_) {
        state_ = make_state(client);
    }

    for (const std::uint16_t port : ports) {
        if (state_->forwarded.count(port) == 0) {
            state_->tunnel->add_forward(port);
            state_->forwarded.insert(port);
        }
    }

    // Point the alias at the sidecar's IP on the network THIS container will
    // be on, joining the sidecar to a user-defined network on first sight.
    std::string ip;
    if (spec.network && *spec.network != "bridge") {
        const std::string name = resolve_network_name(client, *spec.network);
        if (state_->connected_networks.count(name) == 0) {
            try {
                client.connect_network(*spec.network, state_->sidecar_id);
            } catch (const DockerError& e) {
                // Racing another connect (compose, a parallel test) is fine —
                // being on the network is all that matters.
                if (std::string(e.what()).find("already exists") == std::string::npos) {
                    throw;
                }
            }
            state_->connected_networks.insert(name);
        }
        ip = ip_on_network(client, state_->sidecar_id, name, /*fallback_first*/ false);
    } else {
        ip = state_->bridge_ip;
    }
    spec.extra_hosts.push_back(std::string(kHostAccessAlias) + ":" + ip);
}

std::unique_ptr<HostPortForwarder::State> HostPortForwarder::make_state(DockerClient& client) {
    auto state = std::make_unique<State>();

    // The image's own entrypoint+command do all the work: set ${USERNAME}
    // (root) to ${PASSWORD} and run sshd in the foreground with
    // GatewayPorts=yes, so remote forwards bind 0.0.0.0 inside the sidecar
    // (reachable from peer containers), not just its loopback. Only the
    // password env var is injected — overriding the command would fight the
    // `sh -c` entrypoint.
    const std::string password = random_hex(24);
    // NOTE: `testcontainers::tcp` is spelled out — the unqualified name is
    // shadowed by the `asio::ip::tcp` alias in this namespace.
    const ContainerRequest request =
        GenericImage(kSshdImage, kSshdTag)
            .with_exposed_port(testcontainers::tcp(22))
            .with_env("PASSWORD", password)
            .with_wait(wait_for::listening_port(testcontainers::tcp(22)))
            .to_request();
    // Runner::run tags it with the session labels, so Ryuk reaps the sidecar
    // if this process crashes; on a clean exit the handle removes it. The
    // reference emplace() returns keeps the accesses below provably engaged.
    Container& sidecar = state->sidecar.emplace(Runner::run(client, request));
    state->sidecar_id = sidecar.id();
    state->bridge_ip = ip_on_network(client, state->sidecar_id, "bridge", /*fallback_first*/ true);

    // Retry the WHOLE connect+handshake+auth: a userland proxy can accept on
    // the published port before sshd actually listens (the Ryuk lesson), and
    // sshd closes its very first connections while still generating host keys.
    const std::string ssh_host = sidecar.host();
    const std::uint16_t ssh_port = sidecar.get_host_port(testcontainers::tcp(22));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        try {
            state->tunnel = std::make_unique<SshTunnel>(ssh_host, ssh_port, "root", password);
            break;
        } catch (const DockerError&) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    return state;
}

} // namespace testcontainers::detail
