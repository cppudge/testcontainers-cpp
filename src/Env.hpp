#pragma once

#include <cstdlib>
#include <string>

namespace testcontainers::detail {

/// True when the environment variable `name` holds a truthy value ("1",
/// "true", "TRUE", "True") — the set accepted for the library's boolean
/// switches (DOCKER_TLS_VERIFY, TESTCONTAINERS_*). Unset or anything else,
/// including "", is false. Previously copy-pasted per translation unit.
inline bool env_truthy(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr) {
        return false;
    }
    const std::string s(v);
    return s == "1" || s == "true" || s == "TRUE" || s == "True";
}

/// The user's home directory (HOME, else USERPROFILE on Windows); "" if
/// neither is set. The one precedence used everywhere a per-user config file
/// is located (~/.docker, ~/.testcontainers.properties).
inline std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h) {
        return h;
    }
    if (const char* up = std::getenv("USERPROFILE"); up && *up) {
        return up; // Windows
    }
    return {};
}

} // namespace testcontainers::detail
