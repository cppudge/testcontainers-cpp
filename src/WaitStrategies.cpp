#include "WaitStrategies.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <variant>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/Logs.hpp"

namespace testcontainers {
namespace detail {

namespace {

using Clock = std::chrono::steady_clock;

/// Count non-overlapping occurrences of `needle` in `haystack`.
std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

/// Poll the container's logs every ~200ms until `text` has appeared `times`
/// times in the selected stream(s), or the deadline passes (then throw).
void wait_for_log(DockerClient& client, const std::string& id, const wait::LogMessage& cond,
                  Clock::time_point deadline, bool tty) {
    using Source = wait::LogMessage::Source;

    LogOptions opts;
    opts.include_stdout = cond.source != Source::Stderr;
    opts.include_stderr = cond.source != Source::Stdout;
    opts.follow = false;
    opts.tail = "all";
    // A TTY container's log stream is raw/unframed: select the raw decode path so
    // the polled snapshot is searchable instead of garbled multiplex bytes.
    opts.tty = tty;

    const int needed = cond.times < 1 ? 1 : cond.times;

    for (;;) {
        const ContainerLogs logs = client.logs(id, opts);

        std::size_t seen = 0;
        if (cond.source != Source::Stderr) {
            seen += count_occurrences(logs.stdout_data, cond.text);
        }
        if (cond.source != Source::Stdout) {
            seen += count_occurrences(logs.stderr_data, cond.text);
        }
        if (seen >= static_cast<std::size_t>(needed)) {
            return;
        }

        if (Clock::now() >= deadline) {
            throw DockerError("Timed out waiting for log message \"" + cond.text + "\" (" +
                              std::to_string(seen) + "/" + std::to_string(needed) +
                              " occurrences) in container " + id);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

/// Poll `inspect.running` every ~200ms until the container has stopped. If the
/// condition pins an exit code, verify it matches once stopped. Throw on the
/// deadline.
void wait_for_exit(DockerClient& client, const std::string& id, const wait::Exit& cond,
                   Clock::time_point deadline) {
    for (;;) {
        const ContainerInspect info = client.inspect_container(id);
        if (!info.running) {
            if (cond.code) {
                if (info.exit_code != cond.code) {
                    const std::string actual =
                        info.exit_code ? std::to_string(*info.exit_code) : "unknown";
                    throw DockerError("Container " + id + " exited with code " + actual +
                                      ", expected " + std::to_string(*cond.code));
                }
            }
            return;
        }

        if (Clock::now() >= deadline) {
            throw DockerError("Timed out waiting for container " + id + " to exit");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

/// Poll `inspect.health_status` every ~200ms: "healthy" succeeds, "unhealthy"
/// fails fast, a missing status means no healthcheck is configured, and
/// "starting" keeps polling. Throw on the deadline.
void wait_for_healthcheck(DockerClient& client, const std::string& id, Clock::time_point deadline) {
    for (;;) {
        const ContainerInspect info = client.inspect_container(id);
        const std::string status = info.health_status.value_or(std::string{});

        if (status == "healthy") {
            return;
        }
        if (status == "unhealthy") {
            throw DockerError("Container " + id + " reported health status \"unhealthy\"");
        }
        if (status.empty()) {
            throw DockerError("Container " + id +
                              " has no healthcheck configured (cannot wait for healthy)");
        }
        // "starting" (or any other transient status) -> keep polling.

        if (Clock::now() >= deadline) {
            throw DockerError("Timed out waiting for container " + id +
                              " to become healthy (last status \"" + status + "\")");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

/// Resolve the host port Docker published for `port`, preferring the IPv4
/// binding (mirrors `Container::get_host_port`). Throws if nothing is published.
std::uint16_t mapped_host_port(DockerClient& client, const std::string& id,
                               const ContainerPort& port) {
    const ContainerInspect info = client.inspect_container(id);
    const std::string key = to_string(port);

    const auto it = info.ports.find(key);
    if (it == info.ports.end() || it->second.empty()) {
        throw DockerError("Container " + id + " has no published host port for " + key);
    }
    for (const PortBinding& binding : it->second) {
        if (binding.host_ip.find(':') == std::string::npos) { // IPv4 (or empty host IP)
            return binding.host_port;
        }
    }
    return it->second.front().host_port;
}

/// One HTTP probe: open a TCP connection to host:port and GET `path`. Returns
/// the response status, or std::nullopt if the connection/exchange failed (the
/// caller treats that as "not ready yet").
std::optional<int> http_probe(const std::string& host, std::uint16_t port,
                              const std::string& path) {
    namespace asio = boost::asio;
    namespace http = boost::beast::http;
    using asio::ip::tcp;

    try {
        asio::io_context io;
        tcp::resolver resolver(io);
        boost::system::error_code ec;
        const auto endpoints = resolver.resolve(host, std::to_string(port), ec);
        if (ec) {
            return std::nullopt;
        }

        tcp::socket socket(io);
        asio::connect(socket, endpoints, ec);
        if (ec) {
            return std::nullopt; // refused / unreachable -> not ready yet
        }

        http::request<http::empty_body> req{http::verb::get, path, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "testcontainers-cpp");
        req.keep_alive(false);

        http::write(socket, req, ec);
        if (ec) {
            return std::nullopt; // reset mid-write
        }

        boost::beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(socket, buffer, res, ec);
        if (ec && ec != http::error::end_of_stream) {
            return std::nullopt;
        }

        boost::system::error_code ignore;
        socket.shutdown(tcp::socket::shutdown_both, ignore);
        return static_cast<int>(res.result_int());
    } catch (...) {
        // Any transient transport failure is treated as "not ready yet".
        return std::nullopt;
    }
}

/// One TCP probe: resolve host:port and open a connection. Returns true on a
/// successful connect (the caller treats that as "ready"), false on any
/// refusal/unreachable/error (treated as "not ready yet").
bool tcp_probe(const std::string& host, std::uint16_t port) {
    namespace asio = boost::asio;
    using asio::ip::tcp;

    try {
        asio::io_context io;
        tcp::resolver resolver(io);
        boost::system::error_code ec;
        const auto endpoints = resolver.resolve(host, std::to_string(port), ec);
        if (ec) {
            return false;
        }

        tcp::socket socket(io);
        asio::connect(socket, endpoints, ec);
        if (ec) {
            return false; // refused / unreachable -> not ready yet
        }

        boost::system::error_code ignore;
        socket.shutdown(tcp::socket::shutdown_both, ignore);
        return true;
    } catch (...) {
        // Any transient transport failure is treated as "not ready yet".
        return false;
    }
}

/// Resolve the mapped host port once, then probe `cond.path` every
/// `poll_interval` until the response status matches `expected_status`.
/// Connection errors are non-fatal. Throw on the deadline.
void wait_for_http(DockerClient& client, const std::string& id, const wait::Http& cond,
                   Clock::time_point deadline) {
    const std::string host = client.host().http_host();
    const std::uint16_t port = mapped_host_port(client, id, cond.port);

    const std::chrono::milliseconds interval =
        cond.poll_interval.count() > 0 ? cond.poll_interval : std::chrono::milliseconds(200);

    int last_status = 0;
    for (;;) {
        if (const std::optional<int> status = http_probe(host, port, cond.path)) {
            last_status = *status;
            if (last_status == cond.expected_status) {
                return;
            }
        }

        if (Clock::now() >= deadline) {
            throw DockerError("Timed out waiting for HTTP " + std::to_string(cond.expected_status) +
                              " from " + host + ":" + std::to_string(port) + cond.path +
                              " (container " + id + ", last status " + std::to_string(last_status) +
                              ")");
        }
        std::this_thread::sleep_for(interval);
    }
}

/// Resolve the mapped host port once, then attempt a TCP connect every
/// `poll_interval` until it succeeds. Connection errors are non-fatal. Throw on
/// the deadline.
void wait_for_port(DockerClient& client, const std::string& id, const wait::Port& cond,
                   Clock::time_point deadline) {
    const std::string host = client.host().http_host();
    const std::uint16_t port = mapped_host_port(client, id, cond.port);

    const std::chrono::milliseconds interval =
        cond.poll_interval.count() > 0 ? cond.poll_interval : std::chrono::milliseconds(200);

    for (;;) {
        if (tcp_probe(host, port)) {
            return;
        }

        if (Clock::now() >= deadline) {
            throw DockerError("Timed out waiting for TCP connection to " + host + ":" +
                              std::to_string(port) + " (container " + id + ")");
        }
        std::this_thread::sleep_for(interval);
    }
}

} // namespace

void wait_until_ready(DockerClient& client, const std::string& id,
                      const std::vector<WaitFor>& waits, std::chrono::milliseconds timeout,
                      bool tty) {
    const Clock::time_point deadline = Clock::now() + timeout;

    for (const WaitFor& w : waits) {
        std::visit(
            [&](const auto& cond) {
                using T = std::decay_t<decltype(cond)>;
                if constexpr (std::is_same_v<T, wait::None>) {
                    // nothing to do
                } else if constexpr (std::is_same_v<T, wait::LogMessage>) {
                    wait_for_log(client, id, cond, deadline, tty);
                } else if constexpr (std::is_same_v<T, wait::Duration>) {
                    // Clamp the sleep to the shared deadline.
                    const Clock::time_point wake = Clock::now() + cond.value;
                    std::this_thread::sleep_until(wake < deadline ? wake : deadline);
                    if (Clock::now() >= deadline && wake > deadline) {
                        throw DockerError("Timed out during wait::Duration for container " + id);
                    }
                } else if constexpr (std::is_same_v<T, wait::Exit>) {
                    wait_for_exit(client, id, cond, deadline);
                } else if constexpr (std::is_same_v<T, wait::Healthcheck>) {
                    wait_for_healthcheck(client, id, deadline);
                } else if constexpr (std::is_same_v<T, wait::Http>) {
                    wait_for_http(client, id, cond, deadline);
                } else if constexpr (std::is_same_v<T, wait::Port>) {
                    wait_for_port(client, id, cond, deadline);
                }
            },
            w);

        if (Clock::now() >= deadline) {
            // A condition may have just been satisfied at the deadline; only the
            // next condition (if any) would observe the expiry, so re-check here
            // to fail fast rather than enter the next strategy with no budget.
            if (&w != &waits.back()) {
                throw DockerError("Startup timeout exceeded while waiting for container " + id);
            }
        }
    }
}

} // namespace detail
} // namespace testcontainers
