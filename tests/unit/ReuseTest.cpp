#define _CRT_SECURE_NO_WARNINGS // _putenv_s / std::getenv on MSVC

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include "Reuse.hpp"

// Tests in this file:
//   Reuse.HashIsDeterministic - reuse_hash returns the same value for the same input across calls.
//   Reuse.HashDiffersForDifferentInput - reuse_hash maps different inputs to different values.
//   Reuse.HashIs16LowercaseHexChars - reuse_hash always returns exactly 16 lowercase hex characters.
//   Reuse.HashEmptyInputIsFnvOffsetBasis - reuse_hash of the empty string is the 64-bit FNV-1a offset basis.
//   Reuse.HashLabelConstant - reuse_hash_label() is the org.testcontainers.reuse.hash key.
//   Reuse.EnabledViaEnvVar - reuse_enabled() honours TESTCONTAINERS_REUSE_ENABLE truthy values.

using testcontainers::detail::reuse_enabled;
using testcontainers::detail::reuse_hash;
using testcontainers::detail::reuse_hash_label;

namespace {

bool is_lower_hex(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void set_env(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value ? value : ""); // empty value removes it
#else
    if (value) {
        setenv(key, value, 1);
    } else {
        unsetenv(key);
    }
#endif
}

} // namespace

TEST(Reuse, HashIsDeterministic) {
    const std::string in = "{\"Image\":\"alpine:3.20\",\"Cmd\":[\"sleep\",\"120\"]}";
    EXPECT_EQ(reuse_hash(in), reuse_hash(in));
}

TEST(Reuse, HashDiffersForDifferentInput) {
    EXPECT_NE(reuse_hash("alpine:3.20"), reuse_hash("alpine:3.21"));
    EXPECT_NE(reuse_hash("a"), reuse_hash("b"));
    EXPECT_NE(reuse_hash(""), reuse_hash("x"));
}

TEST(Reuse, HashIs16LowercaseHexChars) {
    for (const std::string& in : {std::string(""), std::string("alpine"),
                                  std::string("a much longer canonical config string \n\t with bytes")}) {
        const std::string h = reuse_hash(in);
        EXPECT_EQ(h.size(), 16u) << "input: " << in;
        EXPECT_TRUE(is_lower_hex(h)) << "input: " << in << " -> " << h;
    }
}

TEST(Reuse, HashEmptyInputIsFnvOffsetBasis) {
    // FNV-1a over an empty string is just the 64-bit offset basis
    // (14695981039346656037 == 0xcbf29ce484222325), rendered as 16 hex chars.
    EXPECT_EQ(reuse_hash(""), "cbf29ce484222325");
}

TEST(Reuse, HashLabelConstant) {
    EXPECT_EQ(std::string(reuse_hash_label()), "org.testcontainers.reuse.hash");
}

TEST(Reuse, EnabledViaEnvVar) {
    // Snapshot the current value so the test leaves the environment untouched.
    const char* saved = std::getenv("TESTCONTAINERS_REUSE_ENABLE");
    const std::string previous = saved ? saved : "";
    const bool had_value = saved != nullptr;

    set_env("TESTCONTAINERS_REUSE_ENABLE", "true");
    EXPECT_TRUE(reuse_enabled());
    set_env("TESTCONTAINERS_REUSE_ENABLE", "1");
    EXPECT_TRUE(reuse_enabled());
    // A non-truthy value falls through to the properties file; we don't assert the
    // result here (it depends on whether the host has ~/.testcontainers.properties).

    // Restore the original environment.
    set_env("TESTCONTAINERS_REUSE_ENABLE", had_value ? previous.c_str() : nullptr);
}
