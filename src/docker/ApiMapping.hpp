#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "testcontainers/ExecOptions.hpp"
#include "testcontainers/docker/BuildOptions.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"

// Mapping between our value types and the Docker Engine API JSON. Kept separate
// from DockerClient so it can be unit-tested without a daemon or any networking.
namespace testcontainers::docker {

/// Build the JSON body for `POST /containers/create`.
nlohmann::json build_create_body(const CreateContainerSpec& spec);

/// Build the JSON body for `POST /networks/create` from a NetworkCreateSpec.
nlohmann::json build_network_create_body(const NetworkCreateSpec& spec);

/// Build the JSON body for `POST /networks/{id}/connect`: Container plus an
/// optional EndpointConfig.Aliases (omitted when `aliases` is empty).
nlohmann::json build_connect_network_body(const std::string& container_id,
                                          const std::vector<std::string>& aliases);

/// Build the JSON body for `POST /volumes/create` from a VolumeCreateSpec. Always
/// emits Name; emits Driver/DriverOpts/Labels only when set/non-empty.
nlohmann::json build_volume_create_body(const VolumeCreateSpec& spec);

/// Parse the response of `GET /volumes/{name}` into VolumeInspect. Tolerates a
/// null Labels/Options object (both become empty maps).
VolumeInspect parse_volume_inspect(const std::string& body);

/// Parse the `Os` field from a `GET /version` response body (e.g. "linux" /
/// "windows"). Returns "" if the field is missing. Pure, daemon-free helper so
/// the engine-OS detection in DockerClient can be unit-tested.
std::string parse_server_os(const std::string& version_json);

/// The newest Engine API version this client is written against. Every request
/// is pinned to at most this version so daemon upgrades cannot silently change
/// endpoint behavior underneath us. 1.44 covers everything the library uses
/// (the newest-needed feature, `?platform=` on container create, is 1.41) and
/// is the negotiation floor of daemons that have dropped the older versions.
inline constexpr std::string_view kClientApiVersion = "1.44";

/// Choose the API version to pin requests to: the smaller of kClientApiVersion
/// and `daemon_reported` (the `Api-Version` header of `GET /_ping`), compared
/// numerically per component — "1.9" is OLDER than "1.44". Returns "" when
/// `daemon_reported` is empty or not "major.minor" digits; the caller then
/// falls back to unversioned paths (the daemon's default version).
std::string negotiate_api_version(std::string_view daemon_reported);

/// Build the query string (including the leading '?', or "" when empty) for
/// `POST /containers/create`. Appends `name=` and/or `platform=` when set, each
/// percent-encoded by `encode`. Kept here so the query assembly is unit-testable
/// without a daemon. `encode` is the caller's URL-encoder.
std::string build_create_query(const CreateContainerSpec& spec,
                               const std::function<std::string(const std::string&)>& encode);

/// Parse the response of `GET /containers/{id}/json` into ContainerInspect.
ContainerInspect parse_inspect(const std::string& body);

/// Parse the JSON array from `GET /containers/json` into ContainerSummary list.
std::vector<ContainerSummary> parse_container_list(const std::string& body);

/// Build the JSON body for `POST /containers/{id}/exec` (the exec-create call)
/// from `cmd` and `opts`. Always attaches stdout and stderr so the output can be
/// captured. Emits AttachStdin (only when `opts.stdin_data` is set), Tty, Env
/// (omitted when empty), WorkingDir / User (omitted when nullopt), and Privileged
/// (omitted when false).
nlohmann::json build_exec_create_body(const std::vector<std::string>& cmd,
                                      const ExecOptions& opts = {});

/// Parse the `ExitCode` (integer) from a `GET /exec/{id}/json` response body.
std::int64_t parse_exec_exit_code(const std::string& body);

/// Extract a top-level string `field` from a JSON response `body`, wrapping any
/// parse / missing-field / wrong-type failure in a DockerError (prefixed with
/// `context`) so callers see one uniform error type instead of raw nlohmann
/// exceptions. Used for the create-endpoint "Id"/"Name" extraction.
std::string expect_string_field(const std::string& body, const char* field,
                                const std::string& context);

/// Scan a `POST /images/create` progress stream (newline-delimited JSON) and
/// throw DockerError if it reports an error (Docker returns HTTP 200 even then).
void throw_if_pull_error(const std::string& pull_stream, const std::string& image);

/// Split "name[:tag]" into {name, tag}; tag defaults to "latest". Handles a
/// registry host with a port (e.g. "my-reg:5000/img" -> {"my-reg:5000/img", "latest"}).
std::pair<std::string, std::string> split_image(const std::string& image);

/// Build the query string (incl. leading '?') for `POST /build`: t, dockerfile,
/// nocache, pull, target (when set), and buildargs (a JSON object, percent-encoded).
/// `encode` is the caller's URL-encoder. Unit-testable without a daemon.
std::string build_build_query(const BuildOptions& options,
                              const std::function<std::string(const std::string&)>& encode);

/// Scan a `POST /build` progress stream (newline-delimited JSON) and throw
/// DockerError if any line reports an error ("error"/"errorDetail"). Docker
/// returns HTTP 200 even on build failure (the error is embedded in the stream),
/// exactly like the pull stream. `tag` becomes the error's resource_id().
void throw_if_build_error(const std::string& build_stream, const std::string& tag = {});

} // namespace testcontainers::docker
