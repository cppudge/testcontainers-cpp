#include "testcontainers/version.hpp"

#include <sstream>

// Pull in every third-party dependency so a successful compile+link of this
// translation unit proves the full Conan/CMake toolchain is wired up correctly.
// OpenSSL is in the build only when TLS and/or host-port forwarding is enabled
// (see the TC_TLS / TC_HOST_PORT_FORWARDING options in CMakeLists.txt).
#include <archive.h>
#include <boost/beast/version.hpp>
#include <boost/version.hpp>
#include <nlohmann/json.hpp>

#if defined(TC_TLS) || defined(TC_HOST_PORT_FORWARDING)
#include <openssl/opensslv.h>
#endif

namespace testcontainers {

// TC_VERSION_STRING is stamped on this one translation unit by CMakeLists.txt
// from TC_VERSION_FULL — the single place the version is written.
std::string version() { return TC_VERSION_STRING; }

std::string dependency_report() {
    std::ostringstream os;
    os << "testcontainers-cpp " << version() << '\n';
    os << "  Boost      " << (BOOST_VERSION / 100000) << '.' << (BOOST_VERSION / 100 % 1000) << '.'
       << (BOOST_VERSION % 100) << '\n';
    os << "  Beast      " << BOOST_BEAST_VERSION << '\n';
    os << "  nlohmann   " << NLOHMANN_JSON_VERSION_MAJOR << '.' << NLOHMANN_JSON_VERSION_MINOR
       << '.' << NLOHMANN_JSON_VERSION_PATCH << '\n';
#if defined(TC_TLS) || defined(TC_HOST_PORT_FORWARDING)
    os << "  OpenSSL    " << OPENSSL_VERSION_TEXT << '\n';
#endif
    os << "  libarchive " << archive_version_string() << '\n';
    return os.str();
}

} // namespace testcontainers
