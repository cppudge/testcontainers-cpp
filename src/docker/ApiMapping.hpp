#pragma once

#include <cstdint>
#include <functional>
#include <optional>
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

/// Parse the response of `GET /networks/{id}` into NetworkInspect. Tolerates
/// null / absent Labels, Options, Containers, and IPAM.Config (all become
/// empty containers).
NetworkInspect parse_network_inspect(const std::string& body);

/// Parse the response of `GET /images/{reference}/json` into ImageInspect.
/// Tolerates null / absent RepoTags, RepoDigests, Config and its Labels / Env /
/// Cmd / Entrypoint / ExposedPorts (all become empty containers).
ImageInspect parse_image_inspect(const std::string& body);

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
/// from `cmd` and `opts`. Attaches stdout and stderr so the output can be
/// captured — except with `opts.detach`, which attaches nothing at all (a
/// detached exec streams nothing back; `docker exec -d` parity). Emits
/// AttachStdin (only when `opts.stdin_data` is set and not detaching), Tty, Env
/// (omitted when empty), WorkingDir / User (omitted when nullopt), Privileged
/// (omitted when false), and ConsoleSize as [height, width] (omitted when
/// nullopt).
nlohmann::json build_exec_create_body(const std::vector<std::string>& cmd,
                                      const ExecOptions& opts = {});

/// The subset of `GET /exec/{id}/json` needed to interpret how an exec ended.
struct ExecStatus {
    bool running = false; ///< Running
    /// ExitCode. Absent while the command runs (the daemon reports null) —
    /// but older daemons report 0 instead, so any use of the code must be
    /// gated on `running`, not on presence.
    std::optional<std::int64_t> exit_code;
};

/// Parse `Running` + `ExitCode` from a `GET /exec/{id}/json` response body.
ExecStatus parse_exec_status(const std::string& body);

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
/// nocache, pull, target (when set), and buildargs / labels (JSON objects,
/// percent-encoded). `encode` is the caller's URL-encoder. Unit-testable
/// without a daemon.
std::string build_build_query(const BuildOptions& options,
                              const std::function<std::string(const std::string&)>& encode);

/// Incremental scanner for the `POST /build` progress stream (newline-delimited
/// JSON; Docker answers HTTP 200 even when the build fails, embedding the error
/// in the stream, exactly like the pull stream). feed() it body chunks as they
/// arrive — lines may be split across chunks arbitrarily. Each complete line's
/// "stream" payload is handed to the consumer (when set) and remembered in a
/// bounded tail; an "error"/"errorDetail" line records the failure. finish()
/// scans a trailing unterminated line and, if the build failed, throws
/// DockerError carrying the daemon's message plus the tail of the step output
/// (so the failing RUN's own stdout/stderr is in the exception even when no
/// consumer was attached). `tag` becomes the error's resource_id().
///
/// Pure chunk-in/lines-out logic — unit-testable without a daemon.
class BuildStreamScanner {
public:
    explicit BuildStreamScanner(std::string tag, BuildLogConsumer consumer = {});

    /// Consume the next chunk of the response body.
    void feed(std::string_view chunk);

    /// Flush a trailing unterminated line and throw DockerError if the stream
    /// reported a build error. Call exactly once, after the last feed().
    void finish();

private:
    void scan_line(const std::string& line);

    std::string tag_;
    BuildLogConsumer consumer_;
    std::string pending_;              ///< carry-over of an incomplete trailing line
    std::string tail_;                 ///< bounded tail of "stream" output (error context)
    std::optional<std::string> error_; ///< first "error"/"errorDetail" seen
};

} // namespace testcontainers::docker
