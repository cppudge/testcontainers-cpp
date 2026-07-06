#include <gtest/gtest.h>

#include "testcontainers/version.hpp"

// Tests in this file:
//   Version.IsNotEmpty - the library version string is non-empty.
//   DependencyReport.MentionsEachDependency - the dependency report names Boost, nlohmann, and libarchive always, and OpenSSL exactly when a feature linking it is enabled (TC_TLS / TC_HOST_PORT_FORWARDING).

TEST(Version, IsNotEmpty) { EXPECT_FALSE(testcontainers::version().empty()); }

TEST(DependencyReport, MentionsEachDependency) {
    const std::string report = testcontainers::dependency_report();
    EXPECT_NE(report.find("Boost"), std::string::npos);
    EXPECT_NE(report.find("nlohmann"), std::string::npos);
    EXPECT_NE(report.find("libarchive"), std::string::npos);
    // OpenSSL is linked (and therefore reported) only when TLS and/or
    // host-port forwarding is in the build; a minimal build must NOT claim it.
#if defined(TC_TLS) || defined(TC_HOST_PORT_FORWARDING)
    EXPECT_NE(report.find("OpenSSL"), std::string::npos);
#else
    EXPECT_EQ(report.find("OpenSSL"), std::string::npos);
#endif
}
