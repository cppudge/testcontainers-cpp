#pragma once

#include <optional>
#include <string>

// Pure helpers backing DockerHost::resolve()'s full testcontainers resolution
// order. Each parser takes a file BODY (not a path) so it can be unit-tested
// without touching the filesystem, mirroring ApiMapping / Auth. The actual
// env/file reads + step ordering live in DockerHost.cpp; the
// ~/.testcontainers.properties side lives in Config.hpp (shared parser).
namespace testcontainers::docker {

/// SHA-256 of `data`, lowercase hex (64 chars). Used for the docker-context
/// meta directory name. Self-contained (no OpenSSL dependency).
std::string sha256_hex(const std::string& data);

/// The active docker context name from a ~/.docker/config.json body
/// ("currentContext"); nullopt if absent.
std::optional<std::string> current_context_from_config(const std::string& config_json);

/// One docker-context endpoint parsed from a meta.json body.
struct ContextEndpoint {
    std::string host;             ///< Endpoints.docker.Host, verbatim
    bool skip_tls_verify = false; ///< Endpoints.docker.SkipTLSVerify (absent -> false)
};

/// The docker endpoint from a context meta.json body; nullopt when
/// Endpoints.docker.Host is absent/empty (SkipTLSVerify alone resolves nothing).
std::optional<ContextEndpoint> endpoint_from_context_meta(const std::string& meta_json);

} // namespace testcontainers::docker
