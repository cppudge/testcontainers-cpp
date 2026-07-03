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
/// network / volume / exec instance the failed call was about ("" when the
/// call has no single subject, e.g. list_containers).
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
/// status_code() is always 404.
class NotFoundError : public DockerError {
public:
    explicit NotFoundError(const std::string& what, std::string resource_id = {})
        : DockerError(what, 404, std::move(resource_id)) {}
};

/// A transport operation exceeded its TransportTimeouts deadline (the connect
/// budget or the per-operation io deadline; see docker/Timeouts.hpp). The
/// connection is unusable and has been discarded. No HTTP status exists for a
/// timed-out exchange, so status_code() is std::nullopt.
class TimeoutError : public DockerError {
public:
    explicit TimeoutError(const std::string& what, std::string resource_id = {})
        : DockerError(what, std::nullopt, std::move(resource_id)) {}
};

/// A container/service did not become ready within the caller's startup /
/// wait-strategy timeout. This is a readiness condition of the containerized
/// application, not a Docker failure, so it derives from Error directly —
/// `catch (const DockerError&)` does NOT catch it.
class StartupTimeoutError : public Error {
public:
    using Error::Error;
};

} // namespace testcontainers
