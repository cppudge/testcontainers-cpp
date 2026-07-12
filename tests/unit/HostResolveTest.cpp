#include <gtest/gtest.h>

#include <string>

#include "TestSupport.hpp"
#include "docker/HostResolve.hpp"

// Tests in this file:
//   HostResolve.Sha256EmptyVector - sha256_hex("") matches the standard empty-string digest.
//   HostResolve.Sha256AbcVector - sha256_hex("abc") matches the standard "abc" digest.
//   HostResolve.Sha256PaddingBoundaryVectors - the 55/56/64-byte inputs pin the padding boundaries (length field just fits / spills into a second block / a whole padding block is appended) that single-block vectors never touch.
//   HostResolve.Sha256IsLowercaseHex64 - sha256_hex output is always 64 lowercase-hex characters.
//   HostResolve.ConfigReadsCurrentContext - currentContext is read from a config.json body.
//   HostResolve.ConfigAbsentIsNullopt - a config without currentContext yields nullopt.
//   HostResolve.ConfigInvalidJsonIsNullopt - invalid JSON yields nullopt instead of throwing.
//   HostResolve.ContextMetaReadsDockerHost - Endpoints.docker.Host is read from a realistic meta.json (SkipTLSVerify defaults to false).
//   HostResolve.ContextMetaReadsSkipTlsVerify - Endpoints.docker.SkipTLSVerify=true is carried alongside the host.
//   HostResolve.ContextMetaMissingEndpointsIsNullopt - a meta.json without Endpoints (or without a Host) yields nullopt.

using testcontainers::docker::current_context_from_config;
using testcontainers::docker::endpoint_from_context_meta;
using testcontainers::docker::sha256_hex;

TEST(HostResolve, Sha256EmptyVector) {
    EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(HostResolve, Sha256AbcVector) {
    EXPECT_EQ(sha256_hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(HostResolve, Sha256PaddingBoundaryVectors) {
    // 55 bytes: the 0x80 pad + 8-byte length JUST fit the first block. 56
    // bytes (the NIST two-block vector): the length spills into a second
    // block. 64 bytes: a whole padding block is appended. The single-block
    // vectors above never exercise these branches — the classic sha256
    // regression breaks exactly here while "" and "abc" stay green.
    EXPECT_EQ(sha256_hex(std::string(55, 'a')),
              "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(sha256_hex(std::string(64, 'a')),
              "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST(HostResolve, Sha256IsLowercaseHex64) {
    const std::string digest = sha256_hex("my-ctx");
    EXPECT_EQ(digest.size(), 64u);
    EXPECT_TRUE(tcunit::is_lower_hex(digest));
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
    const auto endpoint = endpoint_from_context_meta(meta);
    ASSERT_TRUE(endpoint.has_value());
    EXPECT_EQ(endpoint->host, "unix:///run/user/1000/docker.sock");
    EXPECT_FALSE(endpoint->skip_tls_verify);
}

TEST(HostResolve, ContextMetaReadsSkipTlsVerify) {
    const std::string meta =
        R"({"Name":"my-ctx","Endpoints":{"docker":{"Host":"tcp://docker:2376","SkipTLSVerify":true}}})";
    const auto endpoint = endpoint_from_context_meta(meta);
    ASSERT_TRUE(endpoint.has_value());
    EXPECT_EQ(endpoint->host, "tcp://docker:2376");
    EXPECT_TRUE(endpoint->skip_tls_verify);
}

TEST(HostResolve, ContextMetaMissingEndpointsIsNullopt) {
    EXPECT_FALSE(endpoint_from_context_meta(R"({"Name":"my-ctx"})").has_value());
    // SkipTLSVerify alone (no Host) resolves nothing.
    EXPECT_FALSE(endpoint_from_context_meta(R"({"Endpoints":{"docker":{"SkipTLSVerify":true}}})")
                     .has_value());
}
