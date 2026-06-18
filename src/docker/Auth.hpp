#pragma once

#include <optional>
#include <string>

#include "testcontainers/RegistryAuth.hpp"

// Docker registry authentication helpers. All functions here are pure (modulo
// the env/file reads in read_docker_auth_config) so the resolution logic can be
// unit-tested without a Docker daemon. Kept separate from DockerClient so it can
// be tested in isolation, mirroring ApiMapping.
namespace testcontainers::docker {

/// Standard base64-encode arbitrary bytes (RFC 4648, with '=' padding).
std::string base64_encode(const std::string& bytes);

/// Standard base64-decode `b64`; ignores embedded whitespace and tolerates
/// missing padding. Returns "" on malformed input.
std::string base64_decode(const std::string& b64);

/// Resolve the registry host for an image reference, mirroring the Docker CLI
/// heuristic: split on the first '/'; the first segment is the registry host
/// only if it contains '.' or ':' (a port) or is "localhost". Otherwise the
/// image lives on Docker Hub and we return the canonical "index.docker.io".
///
/// Examples: "ghcr.io/owner/img" -> "ghcr.io"; "confluentinc/cp-kafka" ->
/// "index.docker.io"; "my-registry.io:5000/image" -> "my-registry.io:5000".
std::string resolve_registry(const std::string& image);

/// Parse a Docker `config.json` string and look up credentials for `registry`.
/// Reads `auths[<registry>]` (also trying the Docker Hub alias
/// "https://index.docker.io/v1/" when registry == "index.docker.io"); decodes a
/// base64 `auth` field into username:password, or uses an `identitytoken`.
/// Returns nullopt when there is no matching entry (or only a credential helper,
/// which is out of scope — see read_docker_auth_config). Sets `server`=registry.
std::optional<RegistryAuth> auth_from_docker_config(const std::string& config_json,
                                                    const std::string& registry);

/// Return the Docker auth config JSON per precedence:
///   1. DOCKER_AUTH_CONFIG env (used directly as the JSON)
///   2. $DOCKER_CONFIG/config.json
///   3. ~/.docker/config.json
/// Returns "{}" when no source is present or readable.
std::string read_docker_auth_config();

/// Build the JSON Docker expects for the `X-Registry-Auth` header and base64-
/// encode it. Uses {"identitytoken":...,"serveraddress":...} when
/// `auth.identity_token` is set, otherwise {"username":...,"password":...,
/// "serveraddress":...}.
std::string encode_x_registry_auth(const RegistryAuth& auth);

/// Convenience: resolve_registry(image) + read_docker_auth_config() +
/// auth_from_docker_config(...). Returns nullopt when no credentials apply.
std::optional<RegistryAuth> resolve_auth_for_image(const std::string& image);

} // namespace testcontainers::docker
