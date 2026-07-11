#include "testcontainers/ConnectionString.hpp"

#include <string>
#include <string_view>

namespace testcontainers {

namespace {

/// Percent-encode a URL component: RFC 3986 unreserved characters
/// (ALPHA / DIGIT / '-' / '.' / '_' / '~') pass through, everything else —
/// '/' and '@' and ':' included — is encoded. Stricter than a query-string
/// encoder on purpose: this runs over userinfo and path components, where
/// those characters are separators.
std::string encode_component(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        const auto u = static_cast<unsigned char>(c);
        const bool unreserved = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
                                (u >= '0' && u <= '9') || u == '-' || u == '.' || u == '_' ||
                                u == '~';
        if (unreserved) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(hex[u >> 4]);
            out.push_back(hex[u & 0x0F]);
        }
    }
    return out;
}

} // namespace

std::string ConnectionString::to_string() const {
    std::string url = scheme_ + "://";

    if (user_ || password_) {
        if (user_) {
            url += encode_component(*user_);
        }
        if (password_) {
            url += ":" + encode_component(*password_);
        }
        url += "@";
    }

    // An IPv6 literal must be bracketed or its colons read as the port
    // separator; an already-bracketed host passes through.
    if (!host_.empty() && host_.find(':') != std::string::npos && host_.front() != '[') {
        url += "[" + host_ + "]";
    } else {
        url += host_;
    }

    if (port_) {
        url += ":" + std::to_string(*port_);
    }
    if (database_) {
        url += "/" + encode_component(*database_);
    }

    char sep = '?';
    for (const auto& [key, value] : params_) {
        url += sep + encode_component(key) + "=" + encode_component(value);
        sep = '&';
    }
    return url;
}

} // namespace testcontainers
