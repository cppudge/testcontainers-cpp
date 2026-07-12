#include "WaitStrategies.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>

#include "Deadline.hpp"
#include "HostAddress.hpp"
#include "docker/Ports.hpp"
#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/ExecOptions.hpp"
#include "testcontainers/ExecResult.hpp"
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

void OccurrenceCounter::feed(std::string_view chunk) {
    if (needle_.empty()) {
        return;
    }
    buffered_.append(chunk.data(), chunk.size());

    std::size_t next = scan_from_;
    std::size_t pos = next;
    while ((pos = buffered_.find(needle_, pos)) != std::string::npos) {
        pos += needle_.size(); // non-overlapping, like count_occurrences
        next = pos;
        ++count_;
    }

    // A future match can start no earlier than (a) right after the last match
    // (its bytes are consumed — non-overlap) and (b) the last needle-1 bytes
    // (anything earlier would already have matched above).
    const std::size_t tail_start =
        buffered_.size() < needle_.size() ? 0 : buffered_.size() - (needle_.size() - 1);
    scan_from_ = std::max(next, tail_start);

    // Trim the consumed prefix lazily so a chatty container's log volume does
    // not accumulate (same compaction idea as LogDemuxer::feed).
    if (scan_from_ > 65536) {
        buffered_.erase(0, scan_from_);
        scan_from_ = 0;
    }
}

using Clock = std::chrono::steady_clock;

namespace {

/// Stream the container's logs (history + follow, one connection) until `text`
/// has appeared `times` times in the selected stream(s), or the deadline
/// passes (then throw). If the stream ends first (the container stopped), a
/// re-follow is retried every ~200ms — a restarting container can still
/// produce the message — and an expired budget always gets one final snapshot
/// check, so even `timeout=0` succeeds when the message is already logged.
void wait_for_log(DockerClient& client, const std::string& id, const wait_for::LogMessage& cond,
                  Clock::time_point deadline, bool tty) {
    using Source = wait_for::LogMessage::Source;

    LogOptions opts;
    opts.include_stdout = cond.source != Source::Stderr;
    opts.include_stderr = cond.source != Source::Stdout;
    opts.tail = "all";
    // A TTY container's log stream is raw/unframed: select the raw decode path
    // so the log text is searchable instead of garbled multiplex bytes.
    opts.tty = tty;

    const auto needed = static_cast<std::size_t>(cond.times < 1 ? 1 : cond.times);

    const auto timeout_error = [&](std::size_t seen) {
        return StartupTimeoutError("Timed out waiting for log message \"" + cond.text + "\" (" +
                                       std::to_string(seen) + "/" + std::to_string(needed) +
                                       " occurrences) in container " + id,
                                   id);
    };

    for (;;) {
        if (Clock::now() >= deadline) {
            // Out of budget — one last snapshot check instead of a stream (the
            // old polling loop also always checked at least once).
            const ContainerLogs logs = client.logs(id, opts);
            std::size_t seen = 0;
            if (cond.source != Source::Stderr) {
                seen += count_occurrences(logs.stdout_data, cond.text);
            }
            if (cond.source != Source::Stdout) {
                seen += count_occurrences(logs.stderr_data, cond.text);
            }
            if (seen >= needed) {
                return;
            }
            throw timeout_error(seen);
        }

        // One follow stream (tail=all: history first, then live output)
        // replaces the old fetch-everything-every-200ms polling. Counters are
        // per source so an interleaved other-stream frame cannot split a
        // match; a re-follow after a stream end recounts from scratch (the
        // history is re-delivered), so the counters reset with it.
        OccurrenceCounter seen_on_stdout(cond.text);
        OccurrenceCounter seen_on_stderr(cond.text);
        const auto seen = [&] { return seen_on_stdout.count() + seen_on_stderr.count(); };

        FollowEnd end{};
        try {
            end = client.follow_logs(
                id, opts,
                [&](LogSource source, std::string_view data) {
                    // The daemon already filters streams via the include
                    // flags, but a TTY stream arrives unattributed (all
                    // "stdout") — keep the client-side source check so a
                    // stderr-only wait cannot match TTY output, exactly like
                    // the snapshot path it replaced.
                    if (source == LogSource::Stdout ? cond.source != Source::Stderr
                                                    : cond.source != Source::Stdout) {
                        (source == LogSource::Stdout ? seen_on_stdout : seen_on_stderr).feed(data);
                    }
                    return seen() < needed && Clock::now() < deadline;
                },
                deadline);
        } catch (const TransportTimeoutError&) {
            // The transport's own deadline fired while connecting or reading
            // the response header. At the wait deadline that IS the readiness
            // timeout; earlier it is a real transport problem — propagate.
            if (Clock::now() >= deadline) {
                throw timeout_error(seen());
            }
            throw;
        }

        if (seen() >= needed) {
            return;
        }
        if (end == FollowEnd::DeadlineExpired || Clock::now() >= deadline) {
            throw timeout_error(seen());
        }
        // FollowEnd::StreamEnded: the container stopped. Its logs are final
        // unless it restarts (restart policy), so pause briefly and re-follow;
        // an expired budget lands in the final snapshot check above.
        std::this_thread::sleep_until(
            std::min(Clock::now() + std::chrono::milliseconds(200),
                     detail::saturated_add(deadline, std::chrono::milliseconds(1))));
    }
}

/// Poll `inspect.running` every ~200ms until the container has stopped. If the
/// condition pins an exit code, verify it matches once stopped. Throw on the
/// deadline.
void wait_for_exit(DockerClient& client, const std::string& id, const wait_for::Exit& cond,
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

    const auto host_port = docker::select_host_port(info.ports, key, docker::HostPortFamily::Any);
    if (!host_port) {
        throw DockerError("Container " + id + " has no published host port for " + key,
                          std::nullopt, id);
    }
    return *host_port;
}

} // namespace

std::chrono::milliseconds probe_budget(std::chrono::steady_clock::time_point deadline) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    return std::clamp(left, std::chrono::milliseconds(1),
                      std::chrono::milliseconds(std::chrono::seconds(5)));
}

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
        stream.async_connect(endpoints, [&](const boost::system::error_code& op_ec,
                                            const tcp::endpoint&) { ec = op_ec; });
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

namespace {

/// One HTTP probe, bounded by `budget`: open a TCP connection to host:port and
/// GET `path`. Returns the response status, or std::nullopt if the
/// connection/exchange failed or timed out (the caller treats that as "not
/// ready yet").
std::optional<int> http_probe(const std::string& host, std::uint16_t port, const std::string& path,
                              std::chrono::milliseconds budget) {
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

        stream.async_connect(endpoints, [&](const boost::system::error_code& op_ec,
                                            const tcp::endpoint&) { ec = op_ec; });
        io.run();
        if (ec) {
            return std::nullopt; // refused / unreachable / timed out -> not ready yet
        }

        http::request<http::empty_body> req{http::verb::get, path, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "testcontainers-cpp");
        req.keep_alive(false);

        http::async_write(stream, req,
                          [&](const boost::system::error_code& op_ec, std::size_t) { ec = op_ec; });
        io.restart();
        io.run();
        if (ec) {
            return std::nullopt; // reset / timed out mid-write
        }

        boost::beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::async_read(stream, buffer, res,
                         [&](const boost::system::error_code& op_ec, std::size_t) { ec = op_ec; });
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

/// Resolve the mapped host port once, then probe `cond.path` every
/// `poll_interval` until the response status matches `expected_status`.
/// Connection errors are non-fatal. Throw on the deadline.
void wait_for_http(DockerClient& client, const std::string& id, const wait_for::Http& cond,
                   Clock::time_point deadline) {
    const std::string host = resolved_host_address(client.host());
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
            throw StartupTimeoutError("Timed out waiting for HTTP " +
                                          std::to_string(cond.expected_status) + " from " + host +
                                          ":" + std::to_string(port) + cond.path + " (container " +
                                          id + ", last status " + std::to_string(last_status) + ")",
                                      id);
        }
        std::this_thread::sleep_for(interval);
    }
}

/// Resolve the mapped host port once, then attempt a TCP connect every
/// `poll_interval` until it succeeds. Connection errors are non-fatal. Throw on
/// the deadline.
void wait_for_port(DockerClient& client, const std::string& id, const wait_for::Port& cond,
                   Clock::time_point deadline) {
    const std::string host = resolved_host_address(client.host());
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

/// Run `cond.cmd` inside the container via a deadline-bounded exec until an
/// attempt exits 0, retrying every `poll_interval`. A non-zero exit is "not
/// ready yet"; so is a daemon-side DockerError (the container may still be
/// starting, or restarting under a restart policy) — except a 404, which
/// means the container is gone for good and propagates. The timeout error
/// carries the last COMPLETED attempt's exit code plus a bounded output
/// snippet: the final attempt is routinely cut off mid-run by the deadline
/// and would otherwise mask the informative outcome before it.
void wait_for_command(DockerClient& client, const std::string& id, const wait_for::Command& cond,
                      Clock::time_point deadline) {
    if (cond.cmd.empty()) {
        throw DockerError("wait_for::Command requires a non-empty command", std::nullopt, id);
    }

    std::string display = cond.cmd.front();
    for (std::size_t i = 1; i < cond.cmd.size(); ++i) {
        display += " " + cond.cmd[i];
    }

    const std::chrono::milliseconds interval =
        cond.poll_interval.count() > 0 ? cond.poll_interval : std::chrono::milliseconds(200);

    std::string last_failure = "no attempt completed";
    const auto timeout_error = [&] {
        return StartupTimeoutError("Timed out waiting for command \"" + display +
                                       "\" to succeed in container " + id + " (" + last_failure +
                                       ")",
                                   id);
    };

    for (;;) {
        try {
            // No stdin and no TTY: the attempt needs no half-closable
            // transport, and the output arrives demuxed regardless of how the
            // container itself was created. The consumer never stops early —
            // the exit code needs the stream to reach its end.
            std::string output;
            const ExecStreamResult res = client.exec(
                id, cond.cmd, ExecOptions{},
                [&output](LogSource, std::string_view chunk) {
                    constexpr std::size_t kSnippetCap = 512;
                    if (output.size() < kSnippetCap) {
                        output.append(chunk.substr(0, kSnippetCap - output.size()));
                    }
                    return true;
                },
                deadline);

            if (res.exit_code) {
                if (*res.exit_code == 0) {
                    return; // the command succeeded — ready
                }
                last_failure = "last exit code " + std::to_string(*res.exit_code) +
                               (output.empty() ? std::string{} : ", output: \"" + output + "\"");
            }
            // No exit code: the attempt was cut off (DeadlineExpired, or the
            // rare still-running race right after StreamEnded) — it says
            // nothing about the command, so keep the previous diagnostic.
        } catch (const TransportTimeoutError&) {
            // The transport's own io deadline fired around the exec
            // round-trips. At the wait deadline that IS the readiness
            // timeout; earlier it is a real transport problem — propagate.
            if (Clock::now() >= deadline) {
                throw timeout_error();
            }
            throw;
        } catch (const NotFoundError&) {
            throw; // the container is gone — no retry can succeed
        } catch (const DockerError& e) {
            // e.g. 409 "container is not running" while it is (re)starting:
            // not ready yet — keep retrying under the deadline.
            last_failure = std::string("last error: ") + e.what();
        }

        if (Clock::now() >= deadline) {
            throw timeout_error();
        }
        // Both legs saturate: `interval` is a user knob (poll_interval) and
        // `deadline` may already sit at the clamped far future.
        std::this_thread::sleep_until(
            std::min(detail::saturated_add(Clock::now(), interval),
                     detail::saturated_add(deadline, std::chrono::milliseconds(1))));
    }
}

} // namespace

void wait_until_ready(DockerClient& client, const std::string& id,
                      const std::vector<WaitFor>& waits, std::chrono::milliseconds timeout,
                      bool tty) {
    // Readiness polling re-inspects every ~200ms (the log wait streams over
    // its own follow connection instead); reuse one daemon connection for the
    // polling GETs instead of paying a fresh connect (a TCP/TLS handshake on
    // remote daemons) per poll. Only GETs ride the session (its
    // stale-connection retry is safe there); the command wait's exec POSTs
    // and hijacked start stream open their own connections by design — its
    // exec-inspect GET is what reuses this one. This scoped reuse is the
    // one deviation from the connection-per-request default shared with the
    // Rust reference (bollard pools nothing); see the DockerClient class doc
    // and docs/TODO.md for the analysis.
    const DockerClient::Session session(client);

    // saturated_add, not '+': the startup timeout is a raw user knob, and a
    // "wait forever"-sized value must clamp instead of wrapping into the past
    // (which failed healthy starts instantly — and only on the stdlib whose
    // clock rep is fine enough to overflow).
    const Clock::time_point deadline = detail::saturated_add(Clock::now(), timeout);

    for (const WaitFor& w : waits) {
        std::visit(
            [&](const auto& cond) {
                using T = std::decay_t<decltype(cond)>;
                if constexpr (std::is_same_v<T, wait_for::None>) {
                    // nothing to do
                } else if constexpr (std::is_same_v<T, wait_for::LogMessage>) {
                    wait_for_log(client, id, cond, deadline, tty);
                } else if constexpr (std::is_same_v<T, wait_for::Duration>) {
                    // Sleep clamped to the shared deadline; the plan logic is
                    // pure and unit-tested (clamped_wait_plan).
                    const ClampedWaitPlan plan =
                        clamped_wait_plan(Clock::now(), cond.value, deadline);
                    std::this_thread::sleep_until(plan.wake);
                    if (plan.times_out) {
                        throw StartupTimeoutError(
                            "Timed out during wait_for::Duration for container " + id, id);
                    }
                } else if constexpr (std::is_same_v<T, wait_for::Exit>) {
                    wait_for_exit(client, id, cond, deadline);
                } else if constexpr (std::is_same_v<T, wait_for::Healthcheck>) {
                    wait_for_healthcheck(client, id, deadline);
                } else if constexpr (std::is_same_v<T, wait_for::Http>) {
                    wait_for_http(client, id, cond, deadline);
                } else if constexpr (std::is_same_v<T, wait_for::Port>) {
                    wait_for_port(client, id, cond, deadline);
                } else if constexpr (std::is_same_v<T, wait_for::Command>) {
                    wait_for_command(client, id, cond, deadline);
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
