#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "testcontainers/ContainerPort.hpp"

namespace testcontainers {

/// Readiness conditions: the alternative types a `WaitFor` can hold, plus the
/// convenience factories (below) that build them.
namespace wait_for {

/// No readiness condition — start() returns as soon as the container is started.
struct None {};

/// Wait until a substring appears in the container's logs a number of times.
struct LogMessage {
    /// Which log stream(s) to scan.
    enum class Source { Stdout, Stderr, Either };

    std::string text; ///< substring to look for
    int times = 1;    ///< required number of occurrences
    Source source = Source::Either;
};

/// Wait for a fixed duration after start (a coarse fallback strategy).
struct Duration {
    std::chrono::milliseconds value{0};
};

/// Wait until the container stops, optionally with a specific exit code.
struct Exit {
    std::optional<std::int64_t> code; ///< required exit code, or any if unset
};

/// Wait until the container's Docker health status becomes "healthy".
struct Healthcheck {};

/// Wait until an HTTP GET to the mapped host port returns `expected_status`.
struct Http {
    std::string path = "/";                       ///< request path
    ContainerPort port;                           ///< container port to probe
    int expected_status = 200;                    ///< status that signals readiness
    std::chrono::milliseconds poll_interval{200}; ///< delay between probes
};

/// Wait until the mapped host port for `port` accepts a TCP connection.
struct Port {
    ContainerPort port;                           ///< container port to probe
    std::chrono::milliseconds poll_interval{200}; ///< delay between probes
};

} // namespace wait_for

/// A readiness condition: a closed sum of the small wait strategies above,
/// dispatched with `std::visit`. Copyable so it lives happily in a vector.
using WaitFor = std::variant<wait_for::None, wait_for::LogMessage, wait_for::Duration,
                             wait_for::Exit, wait_for::Healthcheck, wait_for::Http, wait_for::Port>;

/// Convenience factories that build the right `WaitFor` alternative.
namespace wait_for {

/// Wait for `text` to appear `times` times on stdout.
inline WaitFor stdout_message(std::string text, int times = 1) {
    return LogMessage{std::move(text), times, LogMessage::Source::Stdout};
}

/// Wait for `text` to appear `times` times on stderr.
inline WaitFor stderr_message(std::string text, int times = 1) {
    return LogMessage{std::move(text), times, LogMessage::Source::Stderr};
}

/// Wait for `text` to appear on either stream.
inline WaitFor log(std::string text, int times = 1) {
    return LogMessage{std::move(text), times, LogMessage::Source::Either};
}

/// Wait a fixed number of seconds after start.
inline WaitFor seconds(int s) { return Duration{std::chrono::seconds(s)}; }

/// Wait a fixed number of milliseconds after start.
inline WaitFor millis(int ms) { return Duration{std::chrono::milliseconds(ms)}; }

/// Wait until the container stops (with any exit code).
inline WaitFor exit() { return Exit{}; }

/// Wait until the container stops with the given exit code.
inline WaitFor exit_code(std::int64_t code) { return Exit{code}; }

/// Wait until the container's Docker health becomes "healthy".
inline WaitFor healthy() { return Healthcheck{}; }

/// Wait until an HTTP GET to `path` on the mapped host port for `port` returns
/// `status`.
inline WaitFor http(std::string path, ContainerPort port, int status = 200) {
    Http h;
    h.path = std::move(path);
    h.port = port;
    h.expected_status = status;
    return h;
}

/// Wait until the mapped host port for `port` accepts a TCP connection.
inline WaitFor listening_port(ContainerPort port) {
    Port p;
    p.port = port;
    return p;
}

} // namespace wait_for

} // namespace testcontainers
