#define _CRT_SECURE_NO_WARNINGS // std::getenv on MSVC

#include "docker/Transport.hpp"

#include "docker/TlsConfig.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerHost.hpp"

// Asio first so it pulls <winsock2.h> before any <windows.h> below.
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
#include <boost/asio/local/stream_protocol.hpp>
#endif

#if defined(_WIN32)
#include <boost/asio/windows/stream_handle.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace testcontainers::docker {

namespace {

namespace asio = boost::asio;

using Clock = std::chrono::steady_clock;

/// Budget left until `deadline`, clamped at zero (an exhausted budget makes
/// the next bounded operation time out immediately instead of going negative).
std::chrono::milliseconds remaining(Clock::time_point deadline) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    return left > std::chrono::milliseconds::zero() ? left : std::chrono::milliseconds::zero();
}

/// Run `ioc` until the single pending async operation marks `done` or
/// `timeout` expires. On expiry invoke `cancel` and keep running until the
/// handler fires anyway — it may still reference caller-owned buffers, so it
/// must complete before this returns — then report the expiry as
/// asio::error::timed_out (unless the operation genuinely completed while
/// draining, in which case its real result stands). No timeout -> run to
/// completion.
template <class Cancel>
void run_pending(asio::io_context& ioc, const std::optional<std::chrono::milliseconds>& timeout,
                 const bool& done, boost::system::error_code& ec, Cancel cancel) {
    ioc.restart();
    if (!timeout) {
        ioc.run();
        return;
    }
    ioc.run_for(*timeout);
    if (done) {
        return;
    }
    cancel();
    ioc.run();
    if (!done || ec == asio::error::operation_aborted) {
        ec = asio::error::timed_out;
    }
}

/// Deadline-bounded single I/O operation. `initiate(handler)` must start
/// exactly one async operation on `ioc` completing with (error_code, bytes);
/// `cancel` aborts it when the deadline expires.
template <class Initiate, class Cancel>
std::size_t bounded_io(asio::io_context& ioc,
                       const std::optional<std::chrono::milliseconds>& timeout,
                       boost::system::error_code& ec, Initiate initiate, Cancel cancel) {
    bool done = false;
    std::size_t bytes = 0;
    ec = {};
    initiate([&](const boost::system::error_code& op_ec, std::size_t op_bytes) {
        done = true;
        ec = op_ec;
        bytes = op_bytes;
    });
    run_pending(ioc, timeout, done, ec, cancel);
    return bytes;
}

/// Resolve host:port within the remaining connect budget. An IP literal skips
/// the resolver entirely (the common case for a non-localhost DOCKER_HOST).
/// For names, a caveat: resolver.cancel() cannot interrupt an ALREADY-RUNNING
/// getaddrinfo (Asio's worker thread only checks the cancel flag before
/// starting it), so the drain may overrun the budget by the OS resolver time.
/// That is inherent to Asio — do not "fix" the cancel lambda.
asio::ip::tcp::resolver::results_type resolve_endpoints(asio::io_context& ioc,
                                                        const std::string& host,
                                                        std::uint16_t port,
                                                        Clock::time_point deadline) {
    boost::system::error_code literal_ec;
    const asio::ip::address address = asio::ip::make_address(host, literal_ec);
    if (!literal_ec) {
        return asio::ip::tcp::resolver::results_type::create(
            asio::ip::tcp::endpoint(address, port), host, std::to_string(port));
    }

    asio::ip::tcp::resolver resolver(ioc);
    asio::ip::tcp::resolver::results_type endpoints;
    boost::system::error_code ec;
    bool done = false;
    resolver.async_resolve(host, std::to_string(port),
                           [&](const boost::system::error_code& op_ec, auto results) {
                               done = true;
                               ec = op_ec;
                               endpoints = std::move(results);
                           });
    run_pending(ioc, remaining(deadline), done, ec, [&] { resolver.cancel(); });
    if (ec) {
        throw DockerError("Cannot resolve Docker host '" + host + "': " + ec.message());
    }
    return endpoints;
}

/// Shared io_context + io-deadline state for the concrete transports. Each
/// transport owns a private io_context and runs it only from the calling
/// thread (run_pending), keeping the blocking call-and-return surface of
/// ITransport while every operation is individually cancelable.
class TransportBase : public ITransport {
public:
    void set_io_timeout(std::optional<std::chrono::milliseconds> timeout) override {
        io_timeout_ = timeout;
    }

protected:
    explicit TransportBase(std::optional<std::chrono::milliseconds> io_timeout)
        : io_timeout_(io_timeout) {}

    asio::io_context ioc_;
    std::optional<std::chrono::milliseconds> io_timeout_;
};

/// TCP transport (and the basis for the TLS transport below).
class TcpTransport final : public TransportBase {
public:
    TcpTransport(const std::string& host, std::uint16_t port, const TransportTimeouts& timeouts)
        : TransportBase(timeouts.io), socket_(ioc_) {
        const auto deadline = Clock::now() + timeouts.connect;
        const auto endpoints = resolve_endpoints(ioc_, host, port, deadline);

        boost::system::error_code ec;
        bool done = false;
        asio::async_connect(socket_, endpoints,
                            [&](const boost::system::error_code& op_ec,
                                const asio::ip::tcp::endpoint&) {
                                done = true;
                                ec = op_ec;
                            });
        // Deadline expiry CLOSES the socket instead of cancelling it: the
        // multi-endpoint async_connect treats a cancelled per-endpoint attempt
        // as "try the next endpoint" and only stops when the socket is no
        // longer open, so a plain cancel() would let the drain run to the OS
        // connect timeout — or even report success after the budget expired.
        run_pending(ioc_, remaining(deadline), done, ec, [&] {
            boost::system::error_code ignore;
            socket_.close(ignore);
        });
        if (ec) {
            throw DockerError("Cannot connect to Docker at " + host + ":" +
                              std::to_string(port) + ": " + ec.message());
        }
    }

    std::size_t read_some(void* data, std::size_t size, boost::system::error_code& ec) override {
        return bounded_io(
            ioc_, io_timeout_, ec,
            [&](auto&& handler) {
                socket_.async_read_some(asio::buffer(data, size),
                                        std::forward<decltype(handler)>(handler));
            },
            [&] { cancel_pending(); });
    }
    std::size_t write_some(const void* data, std::size_t size,
                           boost::system::error_code& ec) override {
        return bounded_io(
            ioc_, io_timeout_, ec,
            [&](auto&& handler) {
                socket_.async_write_some(asio::buffer(data, size),
                                         std::forward<decltype(handler)>(handler));
            },
            [&] { cancel_pending(); });
    }
    void shutdown_send() override {
        boost::system::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_send, ec);
    }
    bool supports_half_close() const noexcept override { return true; }
    void close() override {
        boost::system::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

private:
    void cancel_pending() {
        boost::system::error_code ignore;
        socket_.cancel(ignore);
    }

    asio::ip::tcp::socket socket_;
};

/// TLS transport over TCP, configured from Docker's standard TLS env vars
/// (DOCKER_CERT_PATH / DOCKER_TLS_VERIFY). Layers an OpenSSL stream on top of a
/// TcpTransport-style connect, mutually authenticating with the daemon when a
/// client cert/key are present.
class TlsTransport final : public TransportBase {
public:
    TlsTransport(const std::string& host, std::uint16_t port, const TransportTimeouts& timeouts)
        : TransportBase(timeouts.io), ctx_(asio::ssl::context::tls_client), stream_(ioc_, ctx_) {
        ctx_.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                         asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                         asio::ssl::context::no_tlsv1_1);

        const std::string cert_dir = docker_cert_path();
        const TlsFiles files = resolve_tls_files(cert_dir);

        if (docker_tls_verify()) {
            ctx_.set_verify_mode(asio::ssl::verify_peer);
            if (!files.ca_cert.empty() && std::filesystem::exists(files.ca_cert)) {
                boost::system::error_code ec;
                ctx_.load_verify_file(files.ca_cert, ec);
                if (ec) {
                    throw DockerError("Cannot load Docker TLS CA certificate '" + files.ca_cert +
                                      "': " + ec.message());
                }
            }
            stream_.set_verify_callback(asio::ssl::host_name_verification(host));
        } else {
            ctx_.set_verify_mode(asio::ssl::verify_none);
        }

        // A client certificate + key (mutual TLS) when both are present.
        if (!files.client_cert.empty() && !files.client_key.empty() &&
            std::filesystem::exists(files.client_cert) &&
            std::filesystem::exists(files.client_key)) {
            try {
                ctx_.use_certificate_file(files.client_cert, asio::ssl::context::pem);
                ctx_.use_private_key_file(files.client_key, asio::ssl::context::pem);
            } catch (const boost::system::system_error& e) {
                throw DockerError("Cannot load Docker TLS client certificate/key from '" +
                                  cert_dir + "': " + e.what());
            }
        }

        const auto deadline = Clock::now() + timeouts.connect;
        const auto endpoints = resolve_endpoints(ioc_, host, port, deadline);

        boost::system::error_code ec;
        bool done = false;
        asio::async_connect(stream_.lowest_layer(), endpoints,
                            [&](const boost::system::error_code& op_ec,
                                const asio::ip::tcp::endpoint&) {
                                done = true;
                                ec = op_ec;
                            });
        // Close-not-cancel on expiry: see TcpTransport — a cancelled range
        // connect just moves on to the next endpoint.
        run_pending(ioc_, remaining(deadline), done, ec, [&] {
            boost::system::error_code ignore;
            stream_.lowest_layer().close(ignore);
        });
        if (ec) {
            throw DockerError("Cannot connect to Docker at " + host + ":" +
                              std::to_string(port) + ": " + ec.message());
        }

        // Server Name Indication: many TLS endpoints need it to select a cert.
        if (!SSL_set_tlsext_host_name(stream_.native_handle(), host.c_str())) {
            throw DockerError("Cannot set TLS SNI host name for " + host);
        }

        done = false;
        stream_.async_handshake(asio::ssl::stream_base::client,
                                [&](const boost::system::error_code& op_ec) {
                                    done = true;
                                    ec = op_ec;
                                });
        run_pending(ioc_, remaining(deadline), done, ec, [&] { cancel_pending(); });
        if (ec) {
            throw DockerError("TLS handshake with " + host + ":" + std::to_string(port) +
                              " failed: " + ec.message());
        }
    }

    std::size_t read_some(void* data, std::size_t size, boost::system::error_code& ec) override {
        return bounded_io(
            ioc_, io_timeout_, ec,
            [&](auto&& handler) {
                stream_.async_read_some(asio::buffer(data, size),
                                        std::forward<decltype(handler)>(handler));
            },
            [&] { cancel_pending(); });
    }
    std::size_t write_some(const void* data, std::size_t size,
                           boost::system::error_code& ec) override {
        return bounded_io(
            ioc_, io_timeout_, ec,
            [&](auto&& handler) {
                stream_.async_write_some(asio::buffer(data, size),
                                         std::forward<decltype(handler)>(handler));
            },
            [&] { cancel_pending(); });
    }
    void shutdown_send() override {
        // SSL has no clean half-close: shutting down the underlying TCP send side
        // would break the encrypted session for the response read still in flight.
        // No-op; callers needing the EOF signal check supports_half_close().
    }
    bool supports_half_close() const noexcept override { return false; }
    void close() override {
        // Best-effort close_notify with a hard cap: teardown of a wedged peer must
        // not hang (eof / stream_truncated from the peer are normal, never thrown).
        constexpr std::chrono::milliseconds kShutdownCap = std::chrono::seconds(5);
        bool done = false;
        boost::system::error_code ec;
        stream_.async_shutdown([&](const boost::system::error_code& op_ec) {
            done = true;
            ec = op_ec;
        });
        run_pending(ioc_, std::min(io_timeout_.value_or(kShutdownCap), kShutdownCap), done, ec,
                    [&] { cancel_pending(); });
        boost::system::error_code ignore;
        stream_.lowest_layer().close(ignore);
    }

private:
    void cancel_pending() {
        boost::system::error_code ignore;
        stream_.lowest_layer().cancel(ignore);
    }

    asio::ssl::context ctx_; // declared before stream_ (it references ctx_)
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
};

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
/// Unix-domain-socket transport (Linux / macOS).
class UnixTransport final : public TransportBase {
public:
    UnixTransport(const std::string& path, const TransportTimeouts& timeouts)
        : TransportBase(timeouts.io), socket_(ioc_) {
        boost::system::error_code ec;
        bool done = false;
        socket_.async_connect(asio::local::stream_protocol::endpoint(path),
                              [&](const boost::system::error_code& op_ec) {
                                  done = true;
                                  ec = op_ec;
                              });
        run_pending(ioc_, timeouts.connect, done, ec, [&] { cancel_pending(); });
        if (ec) {
            throw DockerError("Cannot connect to Docker unix socket '" + path +
                              "': " + ec.message() + ". Is the Docker daemon running?");
        }
    }

    std::size_t read_some(void* data, std::size_t size, boost::system::error_code& ec) override {
        return bounded_io(
            ioc_, io_timeout_, ec,
            [&](auto&& handler) {
                socket_.async_read_some(asio::buffer(data, size),
                                        std::forward<decltype(handler)>(handler));
            },
            [&] { cancel_pending(); });
    }
    std::size_t write_some(const void* data, std::size_t size,
                           boost::system::error_code& ec) override {
        return bounded_io(
            ioc_, io_timeout_, ec,
            [&](auto&& handler) {
                socket_.async_write_some(asio::buffer(data, size),
                                         std::forward<decltype(handler)>(handler));
            },
            [&] { cancel_pending(); });
    }
    void shutdown_send() override {
        boost::system::error_code ec;
        socket_.shutdown(asio::local::stream_protocol::socket::shutdown_send, ec);
    }
    bool supports_half_close() const noexcept override { return true; }
    void close() override {
        boost::system::error_code ec;
        socket_.close(ec);
    }

private:
    void cancel_pending() {
        boost::system::error_code ignore;
        socket_.cancel(ignore);
    }

    asio::local::stream_protocol::socket socket_;
};
#endif // BOOST_ASIO_HAS_LOCAL_SOCKETS

#if defined(_WIN32)
/// Windows named-pipe transport (Docker Desktop's default endpoint).
class NamedPipeTransport final : public TransportBase {
public:
    NamedPipeTransport(const std::string& pipe_path, const TransportTimeouts& timeouts)
        : TransportBase(timeouts.io), handle_(ioc_) {
        const std::string win_path = to_windows_pipe_path(pipe_path);
        // Opening a local pipe is immediate; only a busy pipe (all instances in
        // use) waits. ONE connect budget covers all attempts (per-attempt waits
        // use whatever is left of it, floored at 1ms — 0 would mean
        // NMPWAIT_USE_DEFAULT_WAIT).
        const auto deadline = Clock::now() + timeouts.connect;
        HANDLE handle = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 3; ++attempt) {
            handle = ::CreateFileA(win_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   /*share*/ 0, /*security*/ nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OVERLAPPED, /*template*/ nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                break;
            }
            const DWORD err = ::GetLastError();
            if (err == ERROR_PIPE_BUSY) {
                const DWORD busy_wait_ms = static_cast<DWORD>(
                    std::max<long long>(1, remaining(deadline).count()));
                ::WaitNamedPipeA(win_path.c_str(), busy_wait_ms);
                continue;
            }
            throw DockerError("Cannot open Docker named pipe '" + win_path +
                              "' (Win32 error " + std::to_string(err) +
                              "). Is Docker Desktop running?");
        }
        if (handle == INVALID_HANDLE_VALUE) {
            throw DockerError("Docker named pipe is busy: " + win_path);
        }
        boost::system::error_code ec;
        handle_.assign(handle, ec);
        if (ec) {
            ::CloseHandle(handle);
            throw DockerError("Cannot bind Docker named pipe handle: " + ec.message());
        }
    }

    std::size_t read_some(void* data, std::size_t size, boost::system::error_code& ec) override {
        return bounded_io(
            ioc_, io_timeout_, ec,
            [&](auto&& handler) {
                handle_.async_read_some(asio::buffer(data, size),
                                        std::forward<decltype(handler)>(handler));
            },
            [&] { cancel_pending(); });
    }
    std::size_t write_some(const void* data, std::size_t size,
                           boost::system::error_code& ec) override {
        return bounded_io(
            ioc_, io_timeout_, ec,
            [&](auto&& handler) {
                handle_.async_write_some(asio::buffer(data, size),
                                         std::forward<decltype(handler)>(handler));
            },
            [&] { cancel_pending(); });
    }
    void shutdown_send() override {
        // A Windows named-pipe handle has no half-close primitive (closing it tears
        // down both directions). No-op; callers needing the EOF signal check
        // supports_half_close().
    }
    bool supports_half_close() const noexcept override { return false; }
    void close() override {
        boost::system::error_code ec;
        handle_.close(ec);
    }

private:
    void cancel_pending() {
        boost::system::error_code ignore;
        handle_.cancel(ignore);
    }

    // "//./pipe/docker_engine" -> "\\.\pipe\docker_engine"
    static std::string to_windows_pipe_path(const std::string& path) {
        std::string s = path;
        for (char& c : s) {
            if (c == '/') {
                c = '\\';
            }
        }
        return s;
    }

    asio::windows::stream_handle handle_;
};
#endif // _WIN32

/// The user's home directory (HOME, else USERPROFILE on Windows); "" if neither
/// is set. Same precedence as Auth.cpp / DockerHost.cpp.
std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h) {
        return h;
    }
    if (const char* up = std::getenv("USERPROFILE"); up && *up) {
        return up; // Windows
    }
    return {};
}

} // namespace

TlsFiles resolve_tls_files(const std::string& cert_dir) {
    if (cert_dir.empty()) {
        return {};
    }
    const std::filesystem::path dir(cert_dir);
    TlsFiles files;
    files.ca_cert = (dir / "ca.pem").string();
    files.client_cert = (dir / "cert.pem").string();
    files.client_key = (dir / "key.pem").string();
    return files;
}

std::string docker_cert_path() {
    if (const char* path = std::getenv("DOCKER_CERT_PATH"); path && *path) {
        return path;
    }
    // Docker's documented fallback: when verification is on but no explicit cert
    // path is set, look in ~/.docker.
    if (docker_tls_verify()) {
        if (const std::string home = home_dir(); !home.empty()) {
            return (std::filesystem::path(home) / ".docker").string();
        }
    }
    return {};
}

bool docker_tls_verify() {
    const char* value = std::getenv("DOCKER_TLS_VERIFY");
    if (!value || !*value) {
        return false;
    }
    const std::string v = value;
    return v == "1" || v == "true" || v == "TRUE" || v == "True";
}

std::unique_ptr<ITransport> connect(const DockerHost& host, const TransportTimeouts& timeouts) {
    switch (host.scheme()) {
    case DockerScheme::Unix:
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
        return std::make_unique<UnixTransport>(host.path(), timeouts);
#else
        throw DockerError("Unix socket transport is not supported on this platform");
#endif
    case DockerScheme::NamedPipe:
#if defined(_WIN32)
        return std::make_unique<NamedPipeTransport>(host.path(), timeouts);
#else
        throw DockerError("Named pipe transport is Windows-only");
#endif
    case DockerScheme::Tcp:
        return std::make_unique<TcpTransport>(host.hostname(), host.port(), timeouts);
    case DockerScheme::Https:
        return std::make_unique<TlsTransport>(host.hostname(), host.port(), timeouts);
    }
    throw DockerError("Unknown Docker host scheme");
}

} // namespace testcontainers::docker
