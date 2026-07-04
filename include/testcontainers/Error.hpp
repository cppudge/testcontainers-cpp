#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace testcontainers {

/// Base class for all testcontainers-cpp exceptions.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Failure while talking to the Docker daemon (transport, HTTP, or API error).
///
/// `status_code()` carries the HTTP status when the daemon replied with an
/// error response (std::nullopt for transport-level failures that never got
/// one). `resource_id()` carries the id or name of the container / image /
/// network / volume / exec instance the failed call was about — best-effort:
/// it is "" when the call has no single subject (e.g. list_containers), may be
/// empty on parse- and transport-level failures, and on two-resource calls
/// (connect_network) names the primary one.
///
/// More status-specific subtypes may be introduced over time (e.g. for 409),
/// so do not assume the dynamic type of a non-404 failure is exactly
/// DockerError — dispatch on status_code() instead.
class DockerError : public Error {
public:
    explicit DockerError(const std::string& what) : Error(what) {}
    DockerError(const std::string& what, std::optional<int> status_code,
                std::string resource_id = {})
        : Error(what), status_code_(status_code), resource_id_(std::move(resource_id)) {}

    std::optional<int> status_code() const noexcept { return status_code_; }
    const std::string& resource_id() const noexcept { return resource_id_; }

private:
    std::optional<int> status_code_;
    std::string resource_id_;
};

/// The daemon replied 404: the referenced resource does not exist.
/// status_code() is always 404. Caveat for image pulls: the registry
/// deliberately answers "not found" for private images requested without
/// credentials, so a pull NotFoundError can also mean "authentication
/// required".
class NotFoundError : public DockerError {
public:
    explicit NotFoundError(const std::string& what, std::string resource_id = {})
        : DockerError(what, 404, std::move(resource_id)) {}
};

/// A transport operation exceeded its deadline — the TransportTimeouts
/// connect budget or per-operation io deadline (see docker/Timeouts.hpp), or
/// an internal handshake budget (the Ryuk registration). The connection is
/// unusable and has been discarded. No HTTP status exists for a timed-out
/// exchange, so status_code() is std::nullopt. Distinct from
/// StartupTimeoutError, which is about the container's readiness, not the
/// daemon connection.
class TransportTimeoutError : public DockerError {
public:
    explicit TransportTimeoutError(const std::string& what, std::string resource_id = {})
        : DockerError(what, std::nullopt, std::move(resource_id)) {}
};

/// A container/service did not become ready within the caller's startup /
/// wait-strategy timeout. This is a readiness condition of the containerized
/// application, not a Docker failure, so it derives from Error directly —
/// `catch (const DockerError&)` does NOT catch it.
class StartupTimeoutError : public Error {
public:
    explicit StartupTimeoutError(const std::string& what, std::string resource_id = {})
        : Error(what), resource_id_(std::move(resource_id)) {}

    /// The container id (wait strategies) or compose service name (exposed-
    /// service probes) that failed to become ready.
    const std::string& resource_id() const noexcept { return resource_id_; }

private:
    std::string resource_id_;
};

} // namespace testcontainers
