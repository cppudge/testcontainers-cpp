#pragma once

#include <optional>
#include <string>

// Pure helpers backing DockerHost::resolve()'s full testcontainers resolution
// order. Each parser takes a file BODY (not a path) so it can be unit-tested
// without touching the filesystem, mirroring ApiMapping / Auth. The actual
// env/file reads + step ordering live in DockerHost.cpp.
namespace testcontainers::docker {

/// SHA-256 of `data`, lowercase hex (64 chars). Used for the docker-context
/// meta directory name. Self-contained (no OpenSSL dependency).
std::string sha256_hex(const std::string& data);

/// Extract `docker.host` from a Java-properties file body (key=value lines,
/// '#'/'!' comments, surrounding spaces trimmed). nullopt if absent/empty.
std::optional<std::string> docker_host_from_properties(const std::string& properties_body);

/// The active docker context name from a ~/.docker/config.json body
/// ("currentContext"); nullopt if absent.
std::optional<std::string> current_context_from_config(const std::string& config_json);

/// The docker endpoint host from a context meta.json body
/// (Endpoints.docker.Host); nullopt if absent/empty.
std::optional<std::string> docker_host_from_context_meta(const std::string& meta_json);

} // namespace testcontainers::docker
