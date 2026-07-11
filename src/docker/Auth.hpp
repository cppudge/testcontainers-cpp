#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>

#include "testcontainers/RegistryAuth.hpp"

// Docker registry authentication helpers. All functions here are pure (modulo
// the env/file reads in read_docker_auth_config, and the process-wide
// credential-helper cache) so the resolution logic can be unit-tested without
// a Docker daemon. Kept separate from DockerClient so it can be tested in
// isolation, mirroring ApiMapping.
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
/// Returns nullopt when there is no matching plaintext entry (a credential
/// helper, if configured, is resolved separately — see
/// select_credential_helper / auth_from_credential_helper). Sets
/// `server`=registry.
std::optional<RegistryAuth> auth_from_docker_config(const std::string& config_json,
                                                    const std::string& registry);

/// Pick the credential helper for `registry` from a parsed Docker config: a
/// per-registry `credHelpers[<registry>]` wins, else the global `credsStore`.
/// Returns the helper NAME (e.g. "desktop") or nullopt. Pure (no subprocess).
/// For the Hub registry ("index.docker.io") also matches the
/// "https://index.docker.io/v1/" credHelpers key.
std::optional<std::string> select_credential_helper(const std::string& config_json,
                                                    const std::string& registry);

/// Parse a `docker-credential-<helper> get` stdout JSON
/// ({"ServerURL","Username","Secret"}) into a RegistryAuth. Username "<token>"
/// means Secret is an identity token. Returns nullopt if the JSON is missing
/// creds. Pure.
std::optional<RegistryAuth> parse_credential_helper_output(const std::string& helper_json,
                                                           const std::string& registry);

/// Run `docker-credential-<helper> get` with the registry server URL on stdin
/// and parse the result. Hub uses "https://index.docker.io/v1/" as the URL.
/// Returns nullopt on a non-zero exit / "not found" / unparseable output.
std::optional<RegistryAuth> auth_from_credential_helper(const std::string& helper,
                                                        const std::string& registry);

/// A process-wide TTL cache in front of a credential-helper invocation, keyed
/// by (helper, registry): returns the cached outcome when it is younger than
/// `ttl`, else calls `fetch` and caches whatever it returns — INCLUDING
/// nullopt, because "no credentials here" is the common answer (Docker
/// Desktop routes EVERY registry through credsStore, so an anonymous public
/// pull would otherwise fork the helper each time). `fetch` is a parameter so
/// the cache logic stays unit-testable without spawning a real helper.
/// Concurrent first lookups for one key may both run `fetch` (idempotent;
/// the later insert wins) — the same benign race the server_os cache accepts.
std::optional<RegistryAuth>
auth_from_credential_helper_cached(const std::string& helper, const std::string& registry,
                                   const std::function<std::optional<RegistryAuth>()>& fetch,
                                   std::chrono::milliseconds ttl);

/// Drop every cached credential-helper outcome (unit-test isolation).
void clear_credential_helper_cache();

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

/// Convenience: resolve_registry(image) + read_docker_auth_config(), then a
/// plaintext `auths` lookup (auth_from_docker_config), falling back to a
/// credential helper (select_credential_helper + auth_from_credential_helper,
/// served through the process-wide cache with a 5-minute TTL) when one is
/// configured. Returns nullopt when no credentials apply. Never throws — a
/// missing/odd helper just yields nullopt (anonymous pull).
std::optional<RegistryAuth> resolve_auth_for_image(const std::string& image);

/// Apply a Docker-Hub image-name prefix (e.g. a corporate registry mirror). The
/// prefix is prepended ONLY to Docker Hub images (resolve_registry(image) ==
/// "index.docker.io"); images already qualified with a registry host
/// (ghcr.io/..., localhost:5000/..., my.reg:5000/...) are returned unchanged, as
/// are images that already start with `prefix`. An empty prefix is a no-op.
/// Mirrors testcontainers' PrefixingImageNameSubstitutor.
std::string apply_hub_image_prefix(const std::string& image, const std::string& prefix);

/// Read TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX (else the `hub.image.name.prefix`
/// key of ~/.testcontainers.properties) and apply it via apply_hub_image_prefix.
/// (The config read is the only impurity; keep apply_hub_image_prefix pure.)
std::string substitute_image_name(const std::string& image);

} // namespace testcontainers::docker
