#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace testcontainers {

/// Assembles a connection URL —
/// `scheme://[user[:password]@]host[:port][/database][?key=value&...]` —
/// percent-encoding every component that needs it. The building block for
/// module connection getters (`postgresql://user:pass@localhost:5432/db`),
/// usable on its own for any URL-shaped DSN.
///
/// Components stay out of the URL until set: no user/password → no
/// userinfo "@", no port → no ":port", no database → no path, no params →
/// no "?". user / password / database / parameter keys and values are
/// percent-encoded (RFC 3986 unreserved characters pass through), so
/// credentials with '@', ':' or '/' survive verbatim; the database is one
/// path segment (a '/' inside it is encoded, not a separator). The scheme
/// is emitted as given; so is the host, except an IPv6 literal, which is
/// bracketed automatically. Parameters keep their insertion order.
class ConnectionString {
public:
    explicit ConnectionString(std::string scheme) : scheme_(std::move(scheme)) {}

    ConnectionString& with_user(std::string user) {
        user_ = std::move(user);
        return *this;
    }

    ConnectionString& with_password(std::string password) {
        password_ = std::move(password);
        return *this;
    }

    ConnectionString& with_host(std::string host) {
        host_ = std::move(host);
        return *this;
    }

    ConnectionString& with_port(std::uint16_t port) {
        port_ = port;
        return *this;
    }

    /// The path segment after the host — the database name for the typical
    /// DB DSN (also e.g. an AMQP vhost or a Redis database index).
    ConnectionString& with_database(std::string database) {
        database_ = std::move(database);
        return *this;
    }

    /// Append one `key=value` query parameter (repeatable; order kept).
    ConnectionString& with_param(std::string key, std::string value) {
        params_.emplace_back(std::move(key), std::move(value));
        return *this;
    }

    /// Render the URL from the components set so far.
    std::string to_string() const;

private:
    std::string scheme_;
    std::optional<std::string> user_;
    std::optional<std::string> password_;
    std::string host_;
    std::optional<std::uint16_t> port_;
    std::optional<std::string> database_;
    std::vector<std::pair<std::string, std::string>> params_;
};

} // namespace testcontainers
