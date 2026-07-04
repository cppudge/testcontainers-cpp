#include "WaitStrategies.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <variant>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>

#include "docker/Ports.hpp"
#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/Logs.hpp"

namespace testcontainers {
namespace detail {

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

namespace {

using Clock = std::chrono::steady_clock;

/// Poll the container's logs every ~200ms until `text` has appeared `times`
/// times in the selected stream(s), or the deadline passes (then throw).
void wait_for_log(DockerClient& client, const std::string& id, const wait::LogMessage& cond,
                  Clock::time_point deadline, bool tty) {
    using Source = wait::LogMessage::Source;

    LogOptions opts;
    opts.include_stdout = cond.source != Source::Stderr;
    opts.include_stderr = cond.source != Source::Stdout;
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
            throw StartupTimeoutError("Timed out waiting for log message \"" + cond.text + "\" (" +
                                          std::to_string(seen) + "/" + std::to_string(needed) +
                                          " occurrences) in container " + id,
                                      id);
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
                                          ", expected " + std::to_string(*cond.code),
                                      std::nullopt, id);
                }
            }
            return;
        }

        if (Clock::now() >= deadline) {
            throw StartupTimeoutError("Timed out waiting for container " + id + " to exit", id);
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
            throw DockerError("Container " + id + " reported health status \"unhealthy\"",
                              std::nullopt, id);
        }
        if (status.empty()) {
            throw DockerError("Container " + id +
                                  " has no healthcheck configured (cannot wait for healthy)",
                              std::nullopt, id);
        }
        // "starting" (or any other transient status) -> keep polling.

        if (Clock::now() >= deadline) {
            throw StartupTimeoutError("Timed out waiting for container " + id +
                                          " to become healthy (last status \"" + status + "\")",
                                      id);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

/// Resolve the host port Docker published for `port`, preferring the IPv4
/// binding (same policy as `Container::get_host_port`). Throws if nothing is
/// published.
std::uint16_t mapped_host_port(DockerClient& client, const std::string& id,
                               const ContainerPort& port) {
    const ContainerInspect info = client.inspect_container(id);
    const std::string key = to_string(port);

    const auto host_port =
        docker::select_host_port(info.ports, key, docker::HostPortFamily::Any);
    if (!host_port) {
        throw DockerError("Container " + id + " has no published host port for " + key,
                          std::nullopt, id);
    }
    return *host_port;
}

/// Per-probe deadline: the time left until `deadline`, capped at 5s and
/// floored at 1ms. A port that ACCEPTS the connection but never answers is an
/// ordinary startup state (listener up, application not serving yet) — an
/// unbounded probe there would hang the whole wait past its own deadline.
///
/// The cap must absorb Windows' refused-connect retry: "localhost" resolves to
/// [::1, 127.0.0.1] and Docker Desktop's proxy listens on IPv4 only, so every
/// probe first burns ~2s on the ::1 attempt (WinSock retries a refused
/// loopback SYN) before 127.0.0.1 succeeds — a 2s cap made every probe time
/// out mid-range-connect and the wait never became ready.
std::chrono::milliseconds probe_budget(Clock::time_point deadline) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    return std::clamp(left, std::chrono::milliseconds(1),
                      std::chrono::milliseconds(std::chrono::seconds(5)));
}

/// One HTTP probe, bounded by `budget`: open a TCP connection to host:port and
/// GET `path`. Returns the response status, or std::nullopt if the
/// connection/exchange failed or timed out (the caller treats that as "not
/// ready yet").
std::optional<int> http_probe(const std::string& host, std::uint16_t port,
                              const std::string& path, std::chrono::milliseconds budget) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
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

        // One expires_after arms the whole probe (connect + write + read): on
        // expiry the pending op fails with beast::error::timeout and the
        // stream's socket is closed — "not ready yet", never a hang.
        beast::tcp_stream stream(io);
        stream.expires_after(budget);

        stream.async_connect(endpoints,
                             [&](const boost::system::error_code& op_ec, const tcp::endpoint&) {
                                 ec = op_ec;
                             });
        io.run();
        if (ec) {
            return std::nullopt; // refused / unreachable / timed out -> not ready yet
        }

        http::request<http::empty_body> req{http::verb::get, path, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "testcontainers-cpp");
        req.keep_alive(false);

        http::async_write(stream, req,
                          [&](const boost::system::error_code& op_ec, std::size_t) {
                              ec = op_ec;
                          });
        io.restart();
        io.run();
        if (ec) {
            return std::nullopt; // reset / timed out mid-write
        }

        boost::beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::async_read(stream, buffer, res,
                         [&](const boost::system::error_code& op_ec, std::size_t) {
                             ec = op_ec;
                         });
        io.restart();
        io.run();
        if (ec && ec != http::error::end_of_stream) {
            return std::nullopt;
        }

        boost::system::error_code ignore;
        stream.socket().shutdown(tcp::socket::shutdown_both, ignore);
        return static_cast<int>(res.result_int());
    } catch (...) {
        // Any transient transport failure is treated as "not ready yet".
        return std::nullopt;
    }
}

/// One TCP probe, bounded by `budget`: resolve host:port and open a
/// connection. Returns true on a successful connect (the caller treats that as
/// "ready"), false on any refusal/unreachable/timeout (treated as "not ready
/// yet").
bool tcp_probe(const std::string& host, std::uint16_t port, std::chrono::milliseconds budget) {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    using asio::ip::tcp;

    try {
        asio::io_context io;
        tcp::resolver resolver(io);
        boost::system::error_code ec;
        const auto endpoints = resolver.resolve(host, std::to_string(port), ec);
        if (ec) {
            return false;
        }

        beast::tcp_stream stream(io);
        stream.expires_after(budget);
        stream.async_connect(endpoints,
                             [&](const boost::system::error_code& op_ec, const tcp::endpoint&) {
                                 ec = op_ec;
                             });
        io.run();
        if (ec) {
            return false; // refused / unreachable / timed out -> not ready yet
        }

        boost::system::error_code ignore;
        stream.socket().shutdown(tcp::socket::shutdown_both, ignore);
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
        if (const std::optional<int> status =
                http_probe(host, port, cond.path, probe_budget(deadline))) {
            last_status = *status;
            if (last_status == cond.expected_status) {
                return;
            }
        }

        if (Clock::now() >= deadline) {
            throw StartupTimeoutError(
                "Timed out waiting for HTTP " + std::to_string(cond.expected_status) + " from " +
                    host + ":" + std::to_string(port) + cond.path + " (container " + id +
                    ", last status " + std::to_string(last_status) + ")",
                id);
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
        if (tcp_probe(host, port, probe_budget(deadline))) {
            return;
        }

        if (Clock::now() >= deadline) {
            throw StartupTimeoutError("Timed out waiting for TCP connection to " + host + ":" +
                                          std::to_string(port) + " (container " + id + ")",
                                      id);
        }
        std::this_thread::sleep_for(interval);
    }
}

} // namespace

void wait_until_ready(DockerClient& client, const std::string& id,
                      const std::vector<WaitFor>& waits, std::chrono::milliseconds timeout,
                      bool tty) {
    // Readiness polling re-inspects / re-fetches logs every ~200ms; reuse one
    // daemon connection for the whole wait instead of paying a fresh connect
    // (a TCP/TLS handshake on remote daemons) per poll. Every daemon call in
    // the polls is a GET, so the session's stale-connection retry is safe.
    // This scoped reuse is the one deviation from the connection-per-request
    // default shared with the Rust reference (bollard pools nothing); see the
    // DockerClient class doc and TODO.md for the analysis.
    const DockerClient::Session session(client);

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
                        throw StartupTimeoutError(
                            "Timed out during wait::Duration for container " + id, id);
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
                throw StartupTimeoutError(
                    "Startup timeout exceeded while waiting for container " + id, id);
            }
        }
    }
}

} // namespace detail
} // namespace testcontainers
