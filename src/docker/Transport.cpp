#define _CRT_SECURE_NO_WARNINGS // std::getenv on MSVC

#include "docker/Transport.hpp"

#include "docker/TlsConfig.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerHost.hpp"

// Asio first so it pulls <winsock2.h> before any <windows.h> below.
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
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

#include <cstdlib>
#include <filesystem>
#include <string>

namespace testcontainers::docker {

namespace {

namespace asio = boost::asio;

/// TCP transport (and the basis for a future TLS transport).
class TcpTransport final : public ITransport {
public:
    TcpTransport(const std::string& host, std::uint16_t port) : socket_(ioc_) {
        boost::system::error_code ec;
        asio::ip::tcp::resolver resolver(ioc_);
        const auto endpoints = resolver.resolve(host, std::to_string(port), ec);
        if (ec) {
            throw DockerError("Cannot resolve Docker host '" + host + "': " + ec.message());
        }
        asio::connect(socket_, endpoints, ec);
        if (ec) {
            throw DockerError("Cannot connect to Docker at " + host + ":" +
                              std::to_string(port) + ": " + ec.message());
        }
    }

    std::size_t read_some(void* data, std::size_t size, boost::system::error_code& ec) override {
        return socket_.read_some(asio::buffer(data, size), ec);
    }
    std::size_t write_some(const void* data, std::size_t size,
                           boost::system::error_code& ec) override {
        return socket_.write_some(asio::buffer(data, size), ec);
    }
    void shutdown_send() override {
        boost::system::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_send, ec);
    }
    void close() override {
        boost::system::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

private:
    asio::io_context ioc_;
    asio::ip::tcp::socket socket_;
};

/// TLS transport over TCP, configured from Docker's standard TLS env vars
/// (DOCKER_CERT_PATH / DOCKER_TLS_VERIFY). Layers an OpenSSL stream on top of a
/// TcpTransport-style connect, mutually authenticating with the daemon when a
/// client cert/key are present.
class TlsTransport final : public ITransport {
public:
    TlsTransport(const std::string& host, std::uint16_t port)
        : ctx_(asio::ssl::context::tls_client), stream_(ioc_, ctx_) {
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

        boost::system::error_code ec;
        asio::ip::tcp::resolver resolver(ioc_);
        const auto endpoints = resolver.resolve(host, std::to_string(port), ec);
        if (ec) {
            throw DockerError("Cannot resolve Docker host '" + host + "': " + ec.message());
        }
        asio::connect(stream_.lowest_layer(), endpoints, ec);
        if (ec) {
            throw DockerError("Cannot connect to Docker at " + host + ":" +
                              std::to_string(port) + ": " + ec.message());
        }

        // Server Name Indication: many TLS endpoints need it to select a cert.
        if (!SSL_set_tlsext_host_name(stream_.native_handle(), host.c_str())) {
            throw DockerError("Cannot set TLS SNI host name for " + host);
        }

        stream_.handshake(asio::ssl::stream_base::client, ec);
        if (ec) {
            throw DockerError("TLS handshake with " + host + ":" + std::to_string(port) +
                              " failed: " + ec.message());
        }
    }

    std::size_t read_some(void* data, std::size_t size, boost::system::error_code& ec) override {
        return stream_.read_some(asio::buffer(data, size), ec);
    }
    std::size_t write_some(const void* data, std::size_t size,
                           boost::system::error_code& ec) override {
        return stream_.write_some(asio::buffer(data, size), ec);
    }
    void shutdown_send() override {
        // SSL has no clean half-close: shutting down the underlying TCP send side
        // would break the encrypted session for the response read still in flight.
        // Best-effort no-op — exec-stdin over a TLS daemon may therefore hang for
        // readers that wait for EOF. A TLS daemon is rare; this is acceptable.
    }
    void close() override {
        // Best-effort: a TLS shutdown commonly returns eof / stream_truncated
        // (the peer closed without a close_notify) — that is normal, never throw.
        boost::system::error_code ec;
        stream_.shutdown(ec);
        stream_.lowest_layer().close(ec);
    }

private:
    asio::io_context ioc_;
    asio::ssl::context ctx_; // declared before stream_ (it references ctx_)
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
};

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
/// Unix-domain-socket transport (Linux / macOS).
class UnixTransport final : public ITransport {
public:
    explicit UnixTransport(const std::string& path) : socket_(ioc_) {
        boost::system::error_code ec;
        socket_.connect(asio::local::stream_protocol::endpoint(path), ec);
        if (ec) {
            throw DockerError("Cannot connect to Docker unix socket '" + path +
                              "': " + ec.message() + ". Is the Docker daemon running?");
        }
    }

    std::size_t read_some(void* data, std::size_t size, boost::system::error_code& ec) override {
        return socket_.read_some(asio::buffer(data, size), ec);
    }
    std::size_t write_some(const void* data, std::size_t size,
                           boost::system::error_code& ec) override {
        return socket_.write_some(asio::buffer(data, size), ec);
    }
    void shutdown_send() override {
        boost::system::error_code ec;
        socket_.shutdown(asio::local::stream_protocol::socket::shutdown_send, ec);
    }
    void close() override {
        boost::system::error_code ec;
        socket_.close(ec);
    }

private:
    asio::io_context ioc_;
    asio::local::stream_protocol::socket socket_;
};
#endif // BOOST_ASIO_HAS_LOCAL_SOCKETS

#if defined(_WIN32)
/// Windows named-pipe transport (Docker Desktop's default endpoint).
class NamedPipeTransport final : public ITransport {
public:
    explicit NamedPipeTransport(const std::string& pipe_path) : handle_(ioc_) {
        const std::string win_path = to_windows_pipe_path(pipe_path);
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
                ::WaitNamedPipeA(win_path.c_str(), 10000); // wait up to 10s for a free instance
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
        return handle_.read_some(asio::buffer(data, size), ec);
    }
    std::size_t write_some(const void* data, std::size_t size,
                           boost::system::error_code& ec) override {
        return handle_.write_some(asio::buffer(data, size), ec);
    }
    void shutdown_send() override {
        // A Windows named-pipe handle has no half-close primitive (closing it tears
        // down both directions). Best-effort no-op — exec-stdin on the Windows
        // engine is out of scope; this limitation is documented and acceptable.
    }
    void close() override {
        boost::system::error_code ec;
        handle_.close(ec);
    }

private:
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

    asio::io_context ioc_;
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

std::unique_ptr<ITransport> connect(const DockerHost& host) {
    switch (host.scheme()) {
    case DockerScheme::Unix:
#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
        return std::make_unique<UnixTransport>(host.path());
#else
        throw DockerError("Unix socket transport is not supported on this platform");
#endif
    case DockerScheme::NamedPipe:
#if defined(_WIN32)
        return std::make_unique<NamedPipeTransport>(host.path());
#else
        throw DockerError("Named pipe transport is Windows-only");
#endif
    case DockerScheme::Tcp:
        return std::make_unique<TcpTransport>(host.hostname(), host.port());
    case DockerScheme::Https:
        return std::make_unique<TlsTransport>(host.hostname(), host.port());
    }
    throw DockerError("Unknown Docker host scheme");
}

} // namespace testcontainers::docker
