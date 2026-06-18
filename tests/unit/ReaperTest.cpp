#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "Reaper.hpp"

// Tests in this file:
//   Reaper.SessionIdNonEmptyAndStable - session_id() returns a non-empty value that is identical across calls.
//   Reaper.RyukFilterLine - ryuk_filter_line builds the "label=<key>=<value>\n" line Ryuk expects.
//   Reaper.LabelsContainManagedBy - testcontainers_labels() always carries the managed-by label.
//   Reaper.LabelsContainSessionIdWhenEnabled - testcontainers_labels() carries the session-id label (== session_id()) unless Ryuk is disabled.

using namespace testcontainers::detail;

TEST(Reaper, SessionIdNonEmptyAndStable) {
    const std::string& a = session_id();
    const std::string& b = session_id();
    EXPECT_FALSE(a.empty());
    EXPECT_EQ(a, b);
    // Same backing object every call (process-lifetime singleton).
    EXPECT_EQ(&a, &b);
}

TEST(Reaper, RyukFilterLine) {
    EXPECT_EQ(ryuk_filter_line("k", "v"), "label=k=v\n");
    EXPECT_EQ(ryuk_filter_line("org.testcontainers.session-id", "abc123"),
              "label=org.testcontainers.session-id=abc123\n");
}

TEST(Reaper, LabelsContainManagedBy) {
    const auto labels = testcontainers_labels();
    const bool has_managed_by =
        std::any_of(labels.begin(), labels.end(), [](const auto& kv) {
            return kv.first == "org.testcontainers.managed-by" &&
                   kv.second == "testcontainers";
        });
    EXPECT_TRUE(has_managed_by);
}

TEST(Reaper, LabelsContainSessionIdWhenEnabled) {
    const auto labels = testcontainers_labels();
    const auto it = std::find_if(labels.begin(), labels.end(), [](const auto& kv) {
        return kv.first == "org.testcontainers.session-id";
    });
    if (ryuk_disabled()) {
        // With the reaper off there is no session label to apply.
        EXPECT_EQ(it, labels.end());
    } else {
        ASSERT_NE(it, labels.end());
        EXPECT_EQ(it->second, session_id());
    }
}
