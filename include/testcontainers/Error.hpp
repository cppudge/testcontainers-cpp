#pragma once

#include <stdexcept>

namespace testcontainers {

/// Base class for all testcontainers-cpp exceptions.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Failure while talking to the Docker daemon (transport, HTTP, or API error).
class DockerError : public Error {
public:
    using Error::Error;
};

} // namespace testcontainers
