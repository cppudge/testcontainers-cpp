#include "docker/Transport.hpp"

#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerHost.hpp"

// Asio first so it pulls <winsock2.h> before any <windows.h> below.
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

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
    void close() override {
        boost::system::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

private:
    asio::io_context ioc_;
    asio::ip::tcp::socket socket_;
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

} // namespace

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
        throw DockerError("TLS (https) transport is not implemented yet");
    }
    throw DockerError("Unknown Docker host scheme");
}

} // namespace testcontainers::docker
