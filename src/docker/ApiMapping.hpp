#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "testcontainers/docker/BuildOptions.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"

// Mapping between our value types and the Docker Engine API JSON. Kept separate
// from DockerClient so it can be unit-tested without a daemon or any networking.
namespace testcontainers::docker {

/// Build the JSON body for `POST /containers/create`.
nlohmann::json build_create_body(const CreateContainerSpec& spec);

/// Build the JSON body for `POST /networks/create` from a NetworkCreateSpec.
nlohmann::json build_network_create_body(const NetworkCreateSpec& spec);

/// Parse the `Os` field from a `GET /version` response body (e.g. "linux" /
/// "windows"). Returns "" if the field is missing. Pure, daemon-free helper so
/// the engine-OS detection in DockerClient can be unit-tested.
std::string parse_server_os(const std::string& version_json);

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

/// Build the JSON body for `POST /containers/{id}/exec` (the exec-create call).
/// Always attaches stdout and stderr so the output can be captured.
nlohmann::json build_exec_create_body(const std::vector<std::string>& cmd);

/// Parse the `ExitCode` (integer) from a `GET /exec/{id}/json` response body.
std::int64_t parse_exec_exit_code(const std::string& body);

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
/// exactly like the pull stream.
void throw_if_build_error(const std::string& build_stream);

} // namespace testcontainers::docker
