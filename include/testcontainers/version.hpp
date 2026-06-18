#pragma once

#include <string>

namespace testcontainers {

/// Library version string, e.g. "0.1.0-alpha.0".
std::string version();

/// Human-readable report of the versions of the bundled third-party
/// dependencies (Boost/Beast, nlohmann_json, OpenSSL, libarchive).
/// Primarily a build/link smoke check that the whole toolchain is wired up.
std::string dependency_report();

} // namespace testcontainers
