#pragma once

#include <string>

namespace testcontainers {

/// Credentials for a Docker registry, used to pull private images.
///
/// Sent to the daemon as the `X-Registry-Auth` header on `POST /images/create`.
/// Provide either `username`/`password` (basic auth) or an `identity_token`
/// (OAuth / identity token); the latter takes precedence when non-empty.
///
/// A plain, copyable value type (no Boost/Asio leakage).
struct RegistryAuth {
    std::string username;
    std::string password;
    std::string server;         ///< registry host, e.g. "ghcr.io" or "index.docker.io"
    std::string identity_token; ///< alternative to user/pass (OAuth / identity token)
};

} // namespace testcontainers
