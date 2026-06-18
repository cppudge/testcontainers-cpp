#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "testcontainers/docker/ContainerSpec.hpp"

// Mapping between our value types and the Docker Engine API JSON. Kept separate
// from DockerClient so it can be unit-tested without a daemon or any networking.
namespace testcontainers::docker {

/// Build the JSON body for `POST /containers/create`.
nlohmann::json build_create_body(const CreateContainerSpec& spec);

/// Parse the response of `GET /containers/{id}/json` into ContainerInspect.
ContainerInspect parse_inspect(const std::string& body);

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

} // namespace testcontainers::docker
