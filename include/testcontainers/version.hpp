#pragma once

#include <string>

namespace testcontainers {

/// Library version string — the full semver, including any prerelease
/// suffix (e.g. "1.2.3" or "1.2.3-alpha.4").
std::string version();

/// Human-readable report of the versions of the bundled third-party
/// dependencies (Boost/Beast, nlohmann_json, OpenSSL, libarchive).
/// Primarily a build/link smoke check that the whole toolchain is wired up.
/// The OpenSSL line appears only in builds that link it (i.e. with TLS
/// and/or host-port forwarding enabled — the default).
std::string dependency_report();

} // namespace testcontainers
