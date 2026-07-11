#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace testcontainers {

/// Transport scheme of a resolved Docker daemon endpoint.
enum class DockerScheme {
    Unix,      ///< unix:///var/run/docker.sock
    NamedPipe, ///< npipe:////./pipe/docker_engine
    Tcp,       ///< tcp:// or http://
    Https,     ///< https:// (TLS)
};

/// TLS material paths attached to a resolved endpoint — filled by
/// `DockerHost::resolve()` when the active docker context stores them
/// (`~/.docker/contexts/tls/...`). When present, the TLS transport uses
/// exactly these files and this verify mode; otherwise (env-driven endpoints)
/// the `DOCKER_CERT_PATH` / `DOCKER_TLS_VERIFY` environment configures TLS.
struct TlsMaterials {
    std::string ca_cert;     ///< CA bundle (PEM); empty when the context has none
    std::string client_cert; ///< client certificate (PEM, mutual TLS); empty when none
    std::string client_key;  ///< client private key (PEM, mutual TLS); empty when none
    /// Verify the server certificate against ca_cert. Gated on the store
    /// actually holding a ca.pem: a context with only the client pair presents
    /// it without server verification (there is no trust anchor to check
    /// against), as does one whose meta sets SkipTLSVerify.
    bool verify = false;
};

/// A resolved Docker daemon endpoint: where and how to reach the daemon.
///
/// Resolution order (first hit wins), mirroring testcontainers:
///   1. `DOCKER_HOST` environment variable.
///   2. `docker.host` in `~/.testcontainers.properties`.
///      In both steps a `tcp://` / `http://` / scheme-less host upgrades to
///      `https` when TLS verification is on (`DOCKER_TLS_VERIFY`, or the
///      `docker.tls.verify` properties key) — the docker CLI treats such a
///      host as a TLS endpoint, and the default port moves 2375 -> 2376.
///   3. The active Docker context's endpoint: the context name is
///      `DOCKER_CONTEXT`, else `currentContext` in `~/.docker/config.json`,
///      else "default". A non-"default" name reads `Endpoints.docker.Host`
///      from `~/.docker/contexts/meta/<sha256(name)>/meta.json`. When the
///      context also stores TLS materials
///      (`~/.docker/contexts/tls/<sha256(name)>/docker/*.pem`), they are
///      attached to the resolved host — see tls_materials() — and a `tcp://`
///      endpoint is likewise treated as `https`;
///      `Endpoints.docker.SkipTLSVerify` turns server verification off.
///   4. Platform default: Windows named pipe `//./pipe/docker_engine`;
///      otherwise the rootless sockets `$XDG_RUNTIME_DIR/docker.sock` then
///      `$HOME/.docker/run/docker.sock` (if they exist), else the unix socket
///      `/var/run/docker.sock`.
///
/// Steps 2-4 never throw on a malformed/absent file — they fall through to the
/// next step. A malformed `DOCKER_HOST` (step 1) still throws via parse().
class DockerHost {
public:
    /// Resolve from the environment / platform defaults.
    static DockerHost resolve();

    /// Parse an explicit Docker host URL (`unix://`, `npipe://`, `tcp://`,
    /// `http://`, `https://`). Throws DockerError on an unsupported scheme.
    /// The scheme is taken at face value: parse() never applies the
    /// TLS-verify upgrade or context materials that resolve() does — spell
    /// `https://` explicitly for a TLS endpoint.
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

    /// TLS materials from the docker context that produced this endpoint
    /// (resolve() step 3); nullopt for every other origin, including parse().
    const std::optional<TlsMaterials>& tls_materials() const noexcept { return tls_materials_; }

private:
    DockerScheme scheme_ = DockerScheme::Unix;
    std::string url_;
    std::string path_;
    std::string hostname_;
    std::uint16_t port_ = 0;
    std::optional<TlsMaterials> tls_materials_;
};

} // namespace testcontainers
