#pragma once

#include <chrono>
#include <optional>

namespace testcontainers::docker {

/// Deadlines the transport layer applies to Docker daemon I/O, so a wedged
/// daemon or a network black-hole fails with an error instead of blocking the
/// calling thread forever.
///
/// `connect` bounds establishing the connection as a whole (name resolution +
/// TCP connect + TLS handshake, or waiting for a free named-pipe instance).
///
/// `io` bounds every individual read/write on the established connection. It
/// is an IDLE deadline — the time until the next chunk of bytes — not a
/// whole-response deadline, so slow-but-flowing bodies (image pulls, build
/// output) only need it to exceed the longest silent gap, not the total
/// duration. `std::nullopt` disables it. The streaming call sites that
/// legitimately idle indefinitely (follow_logs, exec attach reads) disable it
/// internally regardless of this setting.
struct TransportTimeouts {
    std::chrono::milliseconds connect = std::chrono::seconds(10);
    std::optional<std::chrono::milliseconds> io = std::chrono::seconds(60);
};

} // namespace testcontainers::docker
