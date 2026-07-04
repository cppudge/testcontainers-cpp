#include <gtest/gtest.h>

#include "testcontainers/version.hpp"

// Tests in this file:
//   Version.IsNotEmpty - the library version string is non-empty.
//   DependencyReport.MentionsEachDependency - the dependency report names Boost, nlohmann, OpenSSL, and libarchive.

TEST(Version, IsNotEmpty) { EXPECT_FALSE(testcontainers::version().empty()); }

TEST(DependencyReport, MentionsEachDependency) {
    const std::string report = testcontainers::dependency_report();
    EXPECT_NE(report.find("Boost"), std::string::npos);
    EXPECT_NE(report.find("nlohmann"), std::string::npos);
    EXPECT_NE(report.find("OpenSSL"), std::string::npos);
    EXPECT_NE(report.find("libarchive"), std::string::npos);
}
