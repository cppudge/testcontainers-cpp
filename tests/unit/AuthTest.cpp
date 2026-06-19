#include <gtest/gtest.h>

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "docker/Auth.hpp"

// Tests in this file:
//   Auth.Base64EncodeKnownVectors - base64_encode matches RFC 4648 vectors and the "user:pass" credential string.
//   Auth.Base64DecodeKnownVectors - base64_decode inverts the known vectors back to the original bytes.
//   Auth.Base64RoundTrip - encoding then decoding a range of byte strings (all remainder lengths) reproduces the input.
//   Auth.Base64DecodeIgnoresWhitespace - base64_decode skips embedded newlines/spaces and still decodes correctly.
//   Auth.ResolveRegistryDockerHubBareRepo - a bare repo like "alpine" resolves to Docker Hub index.docker.io.
//   Auth.ResolveRegistryDockerHubNamespaced - "confluentinc/cp-kafka" (no dot/port) resolves to index.docker.io.
//   Auth.ResolveRegistryExplicitHost - "ghcr.io/owner/img" resolves to the ghcr.io host.
//   Auth.ResolveRegistryHostWithPort - "my-registry.io:5000/image" resolves to the host:port.
//   Auth.ResolveRegistryLocalhost - "localhost:5000/x" resolves to localhost:5000.
//   Auth.AuthFromConfigBasicAuth - a config with a base64 "auth" entry yields the decoded username/password and server.
//   Auth.AuthFromConfigDockerHubAlias - the legacy https://index.docker.io/v1/ key matches when the registry is index.docker.io.
//   Auth.AuthFromConfigIdentityToken - an "identitytoken" entry yields the token (and no user/pass) with the server set.
//   Auth.AuthFromConfigMissingEntry - a registry absent from "auths" yields nullopt.
//   Auth.AuthFromConfigHelperOnly - a config with only credsStore/credHelpers (no usable auths entry) yields nullopt.
//   Auth.AuthFromConfigInvalidJson - malformed config JSON yields nullopt rather than throwing.
//   Auth.EncodeXRegistryAuthBasic - encode_x_registry_auth base64-encodes JSON with username/password/serveraddress.
//   Auth.EncodeXRegistryAuthIdentityToken - encode_x_registry_auth emits identitytoken/serveraddress (no user/pass) when a token is set.
//   Auth.ApplyHubImagePrefixHubImage - a Docker Hub image gets the corporate-mirror prefix prepended.
//   Auth.ApplyHubImagePrefixAddsSeparator - a prefix without a trailing slash gets a '/' inserted before the image.
//   Auth.ApplyHubImagePrefixLeavesQualifiedHost - an image already qualified with a registry host (ghcr.io/...) is unchanged.
//   Auth.ApplyHubImagePrefixLeavesLocalhost - a localhost/registry-with-port image is unchanged.
//   Auth.ApplyHubImagePrefixNotDoubled - an image already starting with the prefix is not prefixed again.
//   Auth.ApplyHubImagePrefixEmptyIsNoOp - an empty prefix returns the image unchanged.
//   Auth.ApplyHubImagePrefixNamespacedHub - a namespaced Hub image (library/redis) still gets the prefix.

using testcontainers::RegistryAuth;
using testcontainers::docker::apply_hub_image_prefix;
using testcontainers::docker::auth_from_docker_config;
using testcontainers::docker::base64_decode;
using testcontainers::docker::base64_encode;
using testcontainers::docker::encode_x_registry_auth;
using testcontainers::docker::resolve_registry;

TEST(Auth, Base64EncodeKnownVectors) {
    // RFC 4648 §10 test vectors.
    EXPECT_EQ(base64_encode(""), "");
    EXPECT_EQ(base64_encode("f"), "Zg==");
    EXPECT_EQ(base64_encode("fo"), "Zm8=");
    EXPECT_EQ(base64_encode("foo"), "Zm9v");
    EXPECT_EQ(base64_encode("foob"), "Zm9vYg==");
    EXPECT_EQ(base64_encode("fooba"), "Zm9vYmE=");
    EXPECT_EQ(base64_encode("foobar"), "Zm9vYmFy");
    // The credential string Docker stores under "auth".
    EXPECT_EQ(base64_encode("user:pass"), "dXNlcjpwYXNz");
}

TEST(Auth, Base64DecodeKnownVectors) {
    EXPECT_EQ(base64_decode(""), "");
    EXPECT_EQ(base64_decode("Zg=="), "f");
    EXPECT_EQ(base64_decode("Zm8="), "fo");
    EXPECT_EQ(base64_decode("Zm9v"), "foo");
    EXPECT_EQ(base64_decode("Zm9vYg=="), "foob");
    EXPECT_EQ(base64_decode("Zm9vYmE="), "fooba");
    EXPECT_EQ(base64_decode("Zm9vYmFy"), "foobar");
    EXPECT_EQ(base64_decode("dXNlcjpwYXNz"), "user:pass");
}

TEST(Auth, Base64RoundTrip) {
    const std::string inputs[] = {
        "",        "a",        "ab",       "abc",      "abcd",
        "abcde",   "abcdef",   "user:pass:with:colons", std::string("\x00\x01\x02\xFF\xFE", 5),
    };
    for (const auto& in : inputs) {
        EXPECT_EQ(base64_decode(base64_encode(in)), in);
    }
}

TEST(Auth, Base64DecodeIgnoresWhitespace) {
    // Docker config "auth" fields are never wrapped, but be lenient anyway.
    EXPECT_EQ(base64_decode("dXNl\ncjpw\r\nYXNz"), "user:pass");
}

TEST(Auth, ResolveRegistryDockerHubBareRepo) {
    EXPECT_EQ(resolve_registry("alpine"), "index.docker.io");
    EXPECT_EQ(resolve_registry("alpine:3.20"), "index.docker.io");
}

TEST(Auth, ResolveRegistryDockerHubNamespaced) {
    EXPECT_EQ(resolve_registry("confluentinc/cp-kafka"), "index.docker.io");
}

TEST(Auth, ResolveRegistryExplicitHost) {
    EXPECT_EQ(resolve_registry("ghcr.io/owner/img"), "ghcr.io");
    EXPECT_EQ(resolve_registry("ghcr.io/owner/img:1.2"), "ghcr.io");
}

TEST(Auth, ResolveRegistryHostWithPort) {
    EXPECT_EQ(resolve_registry("my-registry.io:5000/image"), "my-registry.io:5000");
}

TEST(Auth, ResolveRegistryLocalhost) {
    EXPECT_EQ(resolve_registry("localhost:5000/x"), "localhost:5000");
}

TEST(Auth, AuthFromConfigBasicAuth) {
    // "auth" = base64("user:pass") = "dXNlcjpwYXNz".
    const std::string config = R"({
        "auths": {
            "ghcr.io": { "auth": "dXNlcjpwYXNz" }
        }
    })";
    const auto auth = auth_from_docker_config(config, "ghcr.io");
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(auth->username, "user");
    EXPECT_EQ(auth->password, "pass");
    EXPECT_EQ(auth->server, "ghcr.io");
    EXPECT_TRUE(auth->identity_token.empty());
}

TEST(Auth, AuthFromConfigDockerHubAlias) {
    // Docker stores Hub creds under the legacy "https://index.docker.io/v1/" key.
    const std::string config = R"({
        "auths": {
            "https://index.docker.io/v1/": { "auth": "dXNlcjpwYXNz" }
        }
    })";
    const auto auth = auth_from_docker_config(config, "index.docker.io");
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(auth->username, "user");
    EXPECT_EQ(auth->password, "pass");
    EXPECT_EQ(auth->server, "index.docker.io");
}

TEST(Auth, AuthFromConfigIdentityToken) {
    const std::string config = R"({
        "auths": {
            "ghcr.io": { "identitytoken": "tok-123" }
        }
    })";
    const auto auth = auth_from_docker_config(config, "ghcr.io");
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(auth->identity_token, "tok-123");
    EXPECT_TRUE(auth->username.empty());
    EXPECT_TRUE(auth->password.empty());
    EXPECT_EQ(auth->server, "ghcr.io");
}

TEST(Auth, AuthFromConfigMissingEntry) {
    const std::string config = R"({
        "auths": {
            "ghcr.io": { "auth": "dXNlcjpwYXNz" }
        }
    })";
    EXPECT_FALSE(auth_from_docker_config(config, "quay.io").has_value());
    // No "auths" at all.
    EXPECT_FALSE(auth_from_docker_config(R"({})", "ghcr.io").has_value());
}

TEST(Auth, AuthFromConfigHelperOnly) {
    // credsStore / credHelpers are out of scope: we do not shell out, so a config
    // that only configures a helper yields no usable credentials.
    const std::string config = R"({
        "credsStore": "desktop",
        "credHelpers": { "ghcr.io": "ghcr-login" },
        "auths": { "ghcr.io": {} }
    })";
    EXPECT_FALSE(auth_from_docker_config(config, "ghcr.io").has_value());
}

TEST(Auth, AuthFromConfigInvalidJson) {
    EXPECT_FALSE(auth_from_docker_config("not json {", "ghcr.io").has_value());
}

TEST(Auth, EncodeXRegistryAuthBasic) {
    RegistryAuth auth;
    auth.username = "user";
    auth.password = "pass";
    auth.server = "ghcr.io";

    const std::string header = encode_x_registry_auth(auth);
    const auto json = nlohmann::json::parse(base64_decode(header));
    EXPECT_EQ(json.at("username"), "user");
    EXPECT_EQ(json.at("password"), "pass");
    EXPECT_EQ(json.at("serveraddress"), "ghcr.io");
    EXPECT_FALSE(json.contains("identitytoken"));
}

TEST(Auth, EncodeXRegistryAuthIdentityToken) {
    RegistryAuth auth;
    auth.identity_token = "tok-123";
    auth.server = "ghcr.io";

    const std::string header = encode_x_registry_auth(auth);
    const auto json = nlohmann::json::parse(base64_decode(header));
    EXPECT_EQ(json.at("identitytoken"), "tok-123");
    EXPECT_EQ(json.at("serveraddress"), "ghcr.io");
    EXPECT_FALSE(json.contains("username"));
    EXPECT_FALSE(json.contains("password"));
}

TEST(Auth, ApplyHubImagePrefixHubImage) {
    EXPECT_EQ(apply_hub_image_prefix("redis:7.2", "mirror.corp/"), "mirror.corp/redis:7.2");
}

TEST(Auth, ApplyHubImagePrefixAddsSeparator) {
    // A prefix without a trailing '/' still produces a well-formed reference.
    EXPECT_EQ(apply_hub_image_prefix("redis:7.2", "mirror.corp"), "mirror.corp/redis:7.2");
}

TEST(Auth, ApplyHubImagePrefixLeavesQualifiedHost) {
    EXPECT_EQ(apply_hub_image_prefix("ghcr.io/o/i:1", "mirror.corp/"), "ghcr.io/o/i:1");
}

TEST(Auth, ApplyHubImagePrefixLeavesLocalhost) {
    EXPECT_EQ(apply_hub_image_prefix("localhost:5000/x", "mirror.corp/"), "localhost:5000/x");
}

TEST(Auth, ApplyHubImagePrefixNotDoubled) {
    EXPECT_EQ(apply_hub_image_prefix("mirror.corp/redis:7.2", "mirror.corp/"),
              "mirror.corp/redis:7.2");
}

TEST(Auth, ApplyHubImagePrefixEmptyIsNoOp) {
    EXPECT_EQ(apply_hub_image_prefix("redis:7.2", ""), "redis:7.2");
}

TEST(Auth, ApplyHubImagePrefixNamespacedHub) {
    // "library/redis" has no dot/port in its first segment, so it is a Hub image.
    EXPECT_EQ(apply_hub_image_prefix("library/redis", "mirror.corp/"),
              "mirror.corp/library/redis");
}
