#pragma once

#include <chrono>
#include <string>
#include <variant>

namespace testcontainers {

namespace wait {

/// No readiness condition — start() returns as soon as the container is started.
struct None {};

/// Wait until a substring appears in the container's logs a number of times.
struct LogMessage {
    /// Which log stream(s) to scan.
    enum class Source { Stdout, Stderr, Either };

    std::string text;             ///< substring to look for
    int times = 1;                ///< required number of occurrences
    Source source = Source::Either;
};

/// Wait for a fixed duration after start (a coarse fallback strategy).
struct Duration {
    std::chrono::milliseconds value{0};
};

} // namespace wait

/// A readiness condition: a closed sum of the small wait strategies above,
/// dispatched with `std::visit`. Copyable so it lives happily in a vector.
using WaitFor = std::variant<wait::None, wait::LogMessage, wait::Duration>;

/// Convenience factories that build the right `WaitFor` alternative.
namespace wait_for {

/// Wait for `text` to appear `times` times on stdout.
inline WaitFor stdout_message(std::string text, int times = 1) {
    return wait::LogMessage{std::move(text), times, wait::LogMessage::Source::Stdout};
}

/// Wait for `text` to appear `times` times on stderr.
inline WaitFor stderr_message(std::string text, int times = 1) {
    return wait::LogMessage{std::move(text), times, wait::LogMessage::Source::Stderr};
}

/// Wait for `text` to appear on either stream.
inline WaitFor log(std::string text, int times = 1) {
    return wait::LogMessage{std::move(text), times, wait::LogMessage::Source::Either};
}

/// Wait a fixed number of seconds after start.
inline WaitFor seconds(int s) {
    return wait::Duration{std::chrono::seconds(s)};
}

/// Wait a fixed number of milliseconds after start.
inline WaitFor millis(int ms) {
    return wait::Duration{std::chrono::milliseconds(ms)};
}

} // namespace wait_for

} // namespace testcontainers
