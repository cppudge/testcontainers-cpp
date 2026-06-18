#pragma once

#include <cstdint>
#include <string>

namespace testcontainers {

/// Transport scheme of a resolved Docker daemon endpoint.
enum class DockerScheme {
    Unix,      ///< unix:///var/run/docker.sock
    NamedPipe, ///< npipe:////./pipe/docker_engine
    Tcp,       ///< tcp:// or http://
    Https,     ///< https:// (TLS)
};

/// A resolved Docker daemon endpoint: where and how to reach the daemon.
///
/// Resolution order (subset of the full testcontainers spec, extended later):
///   1. `DOCKER_HOST` environment variable.
///   2. Platform default: Windows named pipe `//./pipe/docker_engine`,
///      otherwise the unix socket `/var/run/docker.sock`.
class DockerHost {
public:
    /// Resolve from the environment / platform defaults.
    static DockerHost resolve();

    /// Parse an explicit Docker host URL (`unix://`, `npipe://`, `tcp://`,
    /// `http://`, `https://`). Throws DockerError on an unsupported scheme.
    static DockerHost parse(const std::string& url);

    DockerScheme scheme() const noexcept { return scheme_; }

    /// Socket path (unix) or pipe path (named pipe). Empty for tcp/https.
    const std::string& path() const noexcept { return path_; }

    /// Host name (tcp/https). Empty for unix/named pipe.
    const std::string& hostname() const noexcept { return hostname_; }

    /// TCP port (tcp/https). 0 for unix/named pipe.
    std::uint16_t port() const noexcept { return port_; }

    /// Value to use for the HTTP `Host` header.
    std::string http_host() const;

    /// The original (normalized) URL, for logging/diagnostics.
    const std::string& to_string() const noexcept { return url_; }

private:
    DockerScheme scheme_ = DockerScheme::Unix;
    std::string url_;
    std::string path_;
    std::string hostname_;
    std::uint16_t port_ = 0;
};

} // namespace testcontainers
