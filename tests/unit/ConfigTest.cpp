#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>

#include "Config.hpp"
#include "TempHome.hpp"
#include "TestEnv.hpp"

// Tests in this file:
//   Config.ParseTrimsKeysAndValues - spaces/tabs around '=' and CRLF endings are trimmed; the first '=' splits, later ones belong to the value.
//   Config.ParseSkipsCommentsBlanksAndNoise - '#'/'!' comment lines, blanks, '='-less lines, and empty keys are skipped; an inline '#' is NOT a comment (java parity).
//   Config.ParseLastDuplicateWins - a repeated key keeps the LAST value (java.util.Properties#load order).
//   Config.ParseKeepsEmptyValues - "key=" parses to an empty string (the lookup layer decides what empty means).
//   Config.ParseRealisticBody - a realistic multi-key ~/.testcontainers.properties body parses in full.
//   ConfigFile.UserPropertyReadsTheHomeFile - user_property returns values from $HOME/.testcontainers.properties; absent keys and empty values are nullopt.
//   ConfigFile.UserPropertyIsCachedUntilCleared - the file is read once; rewrites are invisible until clear_user_properties_cache().
//   ConfigFile.UserPropertyMissingFileIsNullopt - no properties file means nullopt for every key.
//   ConfigFile.ConfigValueEnvWinsElseProperty - a set env var beats the properties key; unset falls through; neither is nullopt.
//   ConfigFile.ConfigTruthyEnvDecidesWhenSet - a set env var decides via {1,true,TRUE,True}, so an explicit env "false"/"0" overrides a file-enabled switch.
//   ConfigFile.ConfigTruthyPropertyJavaParity - without the env var the properties value decides with Boolean.parseBoolean parity: case-insensitive "true" only, "1" is false.

using testcontainers::detail::clear_user_properties_cache;
using testcontainers::detail::config_truthy;
using testcontainers::detail::config_value;
using testcontainers::detail::parse_properties;
using testcontainers::detail::user_property;

TEST(Config, ParseTrimsKeysAndValues) {
    const auto props = parse_properties("  docker.host =  tcp://5.6.7.8:2375  \r\n"
                                        "\tother.key\t=\tvalue\r\n"
                                        "eq.chain=a=b=c\n");
    ASSERT_EQ(props.size(), 3u);
    EXPECT_EQ(props.at("docker.host"), "tcp://5.6.7.8:2375");
    EXPECT_EQ(props.at("other.key"), "value");
    // The FIRST '=' splits key from value; later ones are value bytes.
    EXPECT_EQ(props.at("eq.chain"), "a=b=c");
}

TEST(Config, ParseSkipsCommentsBlanksAndNoise) {
    const auto props = parse_properties("# a comment\n"
                                        "! also a comment\n"
                                        "\n"
                                        "   \n"
                                        "no-separator-line\n"
                                        " = empty key\n"
                                        "real.key=kept\n");
    ASSERT_EQ(props.size(), 1u);
    EXPECT_EQ(props.at("real.key"), "kept");

    // An inline '#' is NOT a comment: java.util.Properties keeps it in the
    // value, and this file is shared with testcontainers-java.
    const auto inline_hash = parse_properties("k=v # not a comment\n");
    EXPECT_EQ(inline_hash.at("k"), "v # not a comment");
}

TEST(Config, ParseLastDuplicateWins) {
    const auto props = parse_properties("dup.key=first\n"
                                        "dup.key=second\n");
    EXPECT_EQ(props.at("dup.key"), "second");
}

TEST(Config, ParseKeepsEmptyValues) {
    const auto props = parse_properties("empty.value=\n");
    ASSERT_EQ(props.count("empty.value"), 1u);
    EXPECT_EQ(props.at("empty.value"), "");
}

TEST(Config, ParseRealisticBody) {
    const auto props = parse_properties("# testcontainers configuration\n"
                                        "docker.host=tcp://1.2.3.4:2375\n"
                                        "testcontainers.reuse.enable=true\n"
                                        "hub.image.name.prefix=mirror.example.com/\n");
    const std::map<std::string, std::string> expected = {
        {"docker.host", "tcp://1.2.3.4:2375"},
        {"testcontainers.reuse.enable", "true"},
        {"hub.image.name.prefix", "mirror.example.com/"},
    };
    EXPECT_EQ(props, expected);
}

// File-backed tests run on the shared temp-HOME fixture (fresh HOME per test,
// properties cache cleared on both ends).
using ConfigFile = tcunit::TempHomeTest;

TEST_F(ConfigFile, UserPropertyReadsTheHomeFile) {
    set_properties("docker.host=tcp://9.8.7.6:2375\n"
                   "empty.value=\n");
    const auto host = user_property("docker.host");
    ASSERT_TRUE(host.has_value());
    EXPECT_EQ(*host, "tcp://9.8.7.6:2375");
    EXPECT_FALSE(user_property("absent.key").has_value());
    // An empty value reads as absent: every consumer treats "" as unset.
    EXPECT_FALSE(user_property("empty.value").has_value());
}

TEST_F(ConfigFile, UserPropertyIsCachedUntilCleared) {
    set_properties("some.key=first\n");
    EXPECT_EQ(user_property("some.key").value_or(""), "first");

    // A rewrite is invisible through the cache...
    write_properties("some.key=second\n");
    EXPECT_EQ(user_property("some.key").value_or(""), "first");

    // ...until the cache is dropped.
    clear_user_properties_cache();
    EXPECT_EQ(user_property("some.key").value_or(""), "second");
}

TEST_F(ConfigFile, UserPropertyMissingFileIsNullopt) {
    EXPECT_FALSE(user_property("docker.host").has_value());
}

TEST_F(ConfigFile, ConfigValueEnvWinsElseProperty) {
    set_properties("test.switch=from-file\n");

    {
        const tctest::ScopedEnv env("TC_CONFIGTEST_SWITCH", "from-env");
        EXPECT_EQ(config_value("TC_CONFIGTEST_SWITCH", "test.switch").value_or(""), "from-env");
    }
    {
        const tctest::ScopedEnv env("TC_CONFIGTEST_SWITCH", std::nullopt);
        EXPECT_EQ(config_value("TC_CONFIGTEST_SWITCH", "test.switch").value_or(""), "from-file");
        EXPECT_FALSE(config_value("TC_CONFIGTEST_SWITCH", "test.other").has_value());
    }
}

TEST_F(ConfigFile, ConfigTruthyEnvDecidesWhenSet) {
    set_properties("test.switch=true\n");

    // A set env var DECIDES — an explicit off must override the file-enabled
    // switch, not fall through to it.
    for (const char* off : {"false", "0", "no"}) {
        const tctest::ScopedEnv env("TC_CONFIGTEST_SWITCH", off);
        EXPECT_FALSE(config_truthy("TC_CONFIGTEST_SWITCH", "test.switch")) << off;
    }
    for (const char* on : {"1", "true", "TRUE", "True"}) {
        const tctest::ScopedEnv env("TC_CONFIGTEST_SWITCH", on);
        EXPECT_TRUE(config_truthy("TC_CONFIGTEST_SWITCH", "test.switch")) << on;
    }
}

TEST_F(ConfigFile, ConfigTruthyPropertyJavaParity) {
    const tctest::ScopedEnv env("TC_CONFIGTEST_SWITCH", std::nullopt);

    // Boolean.parseBoolean parity: case-insensitive "true" enables...
    for (const char* on : {"true", "TRUE", "True", "  true  "}) {
        set_properties(std::string("test.switch=") + on + "\n");
        EXPECT_TRUE(config_truthy("TC_CONFIGTEST_SWITCH", "test.switch")) << on;
    }
    // ...and everything else (including "1", which java parses as false on
    // this shared file) stays off.
    for (const char* off : {"1", "false", "yes", "on"}) {
        set_properties(std::string("test.switch=") + off + "\n");
        EXPECT_FALSE(config_truthy("TC_CONFIGTEST_SWITCH", "test.switch")) << off;
    }
    set_properties("other.key=true\n");
    EXPECT_FALSE(config_truthy("TC_CONFIGTEST_SWITCH", "test.switch"));
}
