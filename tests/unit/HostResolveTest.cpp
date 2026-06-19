#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <string>

#include "docker/HostResolve.hpp"

// Tests in this file:
//   HostResolve.Sha256EmptyVector - sha256_hex("") matches the standard empty-string digest.
//   HostResolve.Sha256AbcVector - sha256_hex("abc") matches the standard "abc" digest.
//   HostResolve.Sha256IsLowercaseHex64 - sha256_hex output is always 64 lowercase-hex characters.
//   HostResolve.PropertiesReadsDockerHost - docker.host is extracted from a simple properties body.
//   HostResolve.PropertiesToleratesCommentsBlanksSpaces - comments, blank lines, and spaces around '=' are handled.
//   HostResolve.PropertiesAbsentIsNullopt - a body without docker.host yields nullopt.
//   HostResolve.PropertiesCommentedOutIsNullopt - a commented-out docker.host line yields nullopt.
//   HostResolve.ConfigReadsCurrentContext - currentContext is read from a config.json body.
//   HostResolve.ConfigAbsentIsNullopt - a config without currentContext yields nullopt.
//   HostResolve.ConfigInvalidJsonIsNullopt - invalid JSON yields nullopt instead of throwing.
//   HostResolve.ContextMetaReadsDockerHost - Endpoints.docker.Host is read from a realistic meta.json.
//   HostResolve.ContextMetaMissingEndpointsIsNullopt - a meta.json without Endpoints yields nullopt.

using testcontainers::docker::current_context_from_config;
using testcontainers::docker::docker_host_from_context_meta;
using testcontainers::docker::docker_host_from_properties;
using testcontainers::docker::sha256_hex;

TEST(HostResolve, Sha256EmptyVector) {
    EXPECT_EQ(sha256_hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(HostResolve, Sha256AbcVector) {
    EXPECT_EQ(sha256_hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(HostResolve, Sha256IsLowercaseHex64) {
    const std::string digest = sha256_hex("my-ctx");
    EXPECT_EQ(digest.size(), 64u);
    EXPECT_TRUE(std::all_of(digest.begin(), digest.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }));
}

TEST(HostResolve, PropertiesReadsDockerHost) {
    const auto host = docker_host_from_properties("docker.host=tcp://1.2.3.4:2375\n");
    ASSERT_TRUE(host.has_value());
    EXPECT_EQ(*host, "tcp://1.2.3.4:2375");
}

TEST(HostResolve, PropertiesToleratesCommentsBlanksSpaces) {
    const std::string body =
        "# a comment\n"
        "\n"
        "  docker.host =  tcp://5.6.7.8:2375  \n"
        "other.key=ignored\n";
    const auto host = docker_host_from_properties(body);
    ASSERT_TRUE(host.has_value());
    EXPECT_EQ(*host, "tcp://5.6.7.8:2375");
}

TEST(HostResolve, PropertiesAbsentIsNullopt) {
    EXPECT_FALSE(docker_host_from_properties("foo=bar\nbaz=qux\n").has_value());
}

TEST(HostResolve, PropertiesCommentedOutIsNullopt) {
    EXPECT_FALSE(docker_host_from_properties("#docker.host=tcp://1.2.3.4:2375\n").has_value());
    EXPECT_FALSE(docker_host_from_properties("!docker.host=tcp://1.2.3.4:2375\n").has_value());
}

TEST(HostResolve, ConfigReadsCurrentContext) {
    const auto ctx = current_context_from_config(R"({"currentContext":"my-ctx"})");
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(*ctx, "my-ctx");
}

TEST(HostResolve, ConfigAbsentIsNullopt) {
    EXPECT_FALSE(current_context_from_config(R"({"auths":{}})").has_value());
}

TEST(HostResolve, ConfigInvalidJsonIsNullopt) {
    EXPECT_FALSE(current_context_from_config("{not json").has_value());
}

TEST(HostResolve, ContextMetaReadsDockerHost) {
    const std::string meta =
        R"({"Name":"my-ctx","Endpoints":{"docker":{"Host":"unix:///run/user/1000/docker.sock"}}})";
    const auto host = docker_host_from_context_meta(meta);
    ASSERT_TRUE(host.has_value());
    EXPECT_EQ(*host, "unix:///run/user/1000/docker.sock");
}

TEST(HostResolve, ContextMetaMissingEndpointsIsNullopt) {
    EXPECT_FALSE(docker_host_from_context_meta(R"({"Name":"my-ctx"})").has_value());
}
