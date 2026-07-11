#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

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
//   Auth.SelectCredentialHelperPerRegistryWins - a credHelpers[<registry>] entry overrides the global credsStore.
//   Auth.SelectCredentialHelperFallsBackToStore - credsStore is used when no per-registry credHelpers entry matches.
//   Auth.SelectCredentialHelperHubAlias - the legacy https://index.docker.io/v1/ credHelpers key matches the Hub registry.
//   Auth.SelectCredentialHelperNoneConfigured - a config with neither credHelpers nor credsStore yields nullopt.
//   Auth.ParseCredentialHelperOutputBasic - a {"ServerURL","Username","Secret"} payload yields username/password/server.
//   Auth.ParseCredentialHelperOutputIdentityToken - a "<token>" username yields an identity token (no user/pass).
//   Auth.ParseCredentialHelperOutputEmpty - empty/blank-credential helper output yields nullopt.
//   Auth.ParseCredentialHelperOutputInvalidJson - malformed helper JSON yields nullopt rather than throwing.
//   Auth.CredentialHelperCacheServesRepeatLookups - a second lookup for the same (helper, registry) within the TTL returns the cached credentials without re-running the fetch.
//   Auth.CredentialHelperCacheCachesAbsence - a nullopt outcome ("no creds" — the anonymous-pull answer) is cached too, so the helper is not re-forked per pull.
//   Auth.CredentialHelperCacheExpires - a zero TTL makes every entry stale on arrival, so each lookup fetches again.
//   Auth.CredentialHelperCacheKeyIsHelperAndRegistry - lookups differing in registry or helper name fetch independently.
//   Auth.EncodeXRegistryAuthBasic - encode_x_registry_auth base64-encodes JSON with username/password/serveraddress.
//   Auth.EncodeXRegistryAuthIdentityToken - encode_x_registry_auth emits identitytoken/serveraddress (no user/pass) when a token is set.
//   Auth.ApplyHubImagePrefixHubImage - a Docker Hub image gets the corporate-mirror prefix prepended.
//   Auth.ApplyHubImagePrefixAddsSeparator - a prefix without a trailing slash gets a '/' inserted before the image.
//   Auth.ApplyHubImagePrefixLeavesQualifiedHost - an image already qualified with a registry host (ghcr.io/...) is unchanged.
//   Auth.ApplyHubImagePrefixLeavesLocalhost - a localhost/registry-with-port image is unchanged.
//   Auth.ApplyHubImagePrefixNotDoubled - an image already starting with the prefix is not prefixed again.
//   Auth.ApplyHubImagePrefixEmptyIsNoOp - an empty prefix returns the image unchanged.
//   Auth.ApplyHubImagePrefixNamespacedHub - a namespaced Hub image (library/redis) still gets the prefix.
//   Auth.SplitImageRefTagAfterLastSlash - split_image_ref splits name:tag on the last ':' after the last '/'; a registry port is not a tag; no tag means "latest".

using testcontainers::RegistryAuth;
using testcontainers::docker::apply_hub_image_prefix;
using testcontainers::docker::auth_from_credential_helper_cached;
using testcontainers::docker::auth_from_docker_config;
using testcontainers::docker::base64_decode;
using testcontainers::docker::base64_encode;
using testcontainers::docker::clear_credential_helper_cache;
using testcontainers::docker::encode_x_registry_auth;
using testcontainers::docker::parse_credential_helper_output;
using testcontainers::docker::resolve_registry;
using testcontainers::docker::select_credential_helper;
using testcontainers::docker::split_image_ref;

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
        "",
        "a",
        "ab",
        "abc",
        "abcd",
        "abcde",
        "abcdef",
        "user:pass:with:colons",
        std::string("\x00\x01\x02\xFF\xFE", 5),
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

TEST(Auth, SelectCredentialHelperPerRegistryWins) {
    // A per-registry credHelpers entry beats the global credsStore.
    const std::string config = R"({
        "credsStore": "global",
        "credHelpers": { "ghcr.io": "ghcr-helper" }
    })";
    const auto helper = select_credential_helper(config, "ghcr.io");
    ASSERT_TRUE(helper.has_value());
    EXPECT_EQ(*helper, "ghcr-helper");
}

TEST(Auth, SelectCredentialHelperFallsBackToStore) {
    // No per-registry helper for quay.io, so the global credsStore is used.
    const std::string config = R"({
        "credsStore": "global",
        "credHelpers": { "ghcr.io": "ghcr-helper" }
    })";
    const auto helper = select_credential_helper(config, "quay.io");
    ASSERT_TRUE(helper.has_value());
    EXPECT_EQ(*helper, "global");
}

TEST(Auth, SelectCredentialHelperHubAlias) {
    // The Hub registry matches the legacy https://index.docker.io/v1/ key.
    const std::string config = R"({
        "credHelpers": { "https://index.docker.io/v1/": "hub-helper" }
    })";
    const auto helper = select_credential_helper(config, "index.docker.io");
    ASSERT_TRUE(helper.has_value());
    EXPECT_EQ(*helper, "hub-helper");
}

TEST(Auth, SelectCredentialHelperNoneConfigured) {
    EXPECT_FALSE(select_credential_helper(R"({"auths":{}})", "ghcr.io").has_value());
}

TEST(Auth, ParseCredentialHelperOutputBasic) {
    const std::string out = R"({"ServerURL":"ghcr.io","Username":"me","Secret":"pw"})";
    const auto auth = parse_credential_helper_output(out, "ghcr.io");
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(auth->username, "me");
    EXPECT_EQ(auth->password, "pw");
    EXPECT_EQ(auth->server, "ghcr.io");
    EXPECT_TRUE(auth->identity_token.empty());
}

TEST(Auth, ParseCredentialHelperOutputIdentityToken) {
    // A "<token>" username means Secret is an identity token, not a password.
    const std::string out =
        R"({"ServerURL":"index.docker.io","Username":"<token>","Secret":"tok"})";
    const auto auth = parse_credential_helper_output(out, "index.docker.io");
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(auth->identity_token, "tok");
    EXPECT_TRUE(auth->username.empty());
    EXPECT_TRUE(auth->password.empty());
    EXPECT_EQ(auth->server, "index.docker.io");
}

TEST(Auth, ParseCredentialHelperOutputEmpty) {
    EXPECT_FALSE(parse_credential_helper_output("", "ghcr.io").has_value());
    EXPECT_FALSE(
        parse_credential_helper_output(R"({"Username":"","Secret":""})", "ghcr.io").has_value());
}

TEST(Auth, ParseCredentialHelperOutputInvalidJson) {
    EXPECT_FALSE(parse_credential_helper_output("not json {", "ghcr.io").has_value());
}

TEST(Auth, CredentialHelperCacheServesRepeatLookups) {
    clear_credential_helper_cache();
    int calls = 0;
    const auto fetch = [&calls]() -> std::optional<RegistryAuth> {
        ++calls;
        RegistryAuth auth;
        auth.username = "me";
        auth.password = "pw";
        auth.server = "ghcr.io";
        return auth;
    };

    const auto first =
        auth_from_credential_helper_cached("helper", "ghcr.io", fetch, std::chrono::hours(1));
    const auto second =
        auth_from_credential_helper_cached("helper", "ghcr.io", fetch, std::chrono::hours(1));
    EXPECT_EQ(calls, 1);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->username, "me");
    EXPECT_EQ(second->password, "pw");
    clear_credential_helper_cache();
}

TEST(Auth, CredentialHelperCacheCachesAbsence) {
    // "No credentials" is the common answer (Docker Desktop routes EVERY
    // registry through credsStore) — it must not re-fork the helper either.
    clear_credential_helper_cache();
    int calls = 0;
    const auto fetch = [&calls]() -> std::optional<RegistryAuth> {
        ++calls;
        return std::nullopt;
    };

    EXPECT_FALSE(auth_from_credential_helper_cached("desktop", "index.docker.io", fetch,
                                                    std::chrono::hours(1))
                     .has_value());
    EXPECT_FALSE(auth_from_credential_helper_cached("desktop", "index.docker.io", fetch,
                                                    std::chrono::hours(1))
                     .has_value());
    EXPECT_EQ(calls, 1);
    clear_credential_helper_cache();
}

TEST(Auth, CredentialHelperCacheExpires) {
    clear_credential_helper_cache();
    int calls = 0;
    const auto fetch = [&calls]() -> std::optional<RegistryAuth> {
        ++calls;
        return std::nullopt;
    };

    // A zero TTL makes every entry stale on arrival: both lookups fetch.
    auth_from_credential_helper_cached("helper", "ghcr.io", fetch, std::chrono::milliseconds(0));
    auth_from_credential_helper_cached("helper", "ghcr.io", fetch, std::chrono::milliseconds(0));
    EXPECT_EQ(calls, 2);
    clear_credential_helper_cache();
}

TEST(Auth, CredentialHelperCacheKeyIsHelperAndRegistry) {
    clear_credential_helper_cache();
    int calls = 0;
    const auto fetch = [&calls]() -> std::optional<RegistryAuth> {
        ++calls;
        return std::nullopt;
    };

    auth_from_credential_helper_cached("helper", "ghcr.io", fetch, std::chrono::hours(1));
    auth_from_credential_helper_cached("helper", "quay.io", fetch, std::chrono::hours(1));
    auth_from_credential_helper_cached("other-helper", "ghcr.io", fetch, std::chrono::hours(1));
    EXPECT_EQ(calls, 3);
    clear_credential_helper_cache();
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
    EXPECT_EQ(apply_hub_image_prefix("library/redis", "mirror.corp/"), "mirror.corp/library/redis");
}

TEST(Auth, SplitImageRefTagAfterLastSlash) {
    using P = std::pair<std::string, std::string>;
    EXPECT_EQ(split_image_ref("redis"), (P{"redis", "latest"}));
    EXPECT_EQ(split_image_ref("redis:"), (P{"redis", "latest"})); // trailing ':' = no tag
    EXPECT_EQ(split_image_ref("redis:7.2"), (P{"redis", "7.2"}));
    EXPECT_EQ(split_image_ref("testcontainers/sshd:1.3.0"), (P{"testcontainers/sshd", "1.3.0"}));
    // A ':' before the last '/' is a registry port, not a tag separator.
    EXPECT_EQ(split_image_ref("localhost:5000/sshd"), (P{"localhost:5000/sshd", "latest"}));
    EXPECT_EQ(split_image_ref("my.reg:5000/ns/sshd:2"), (P{"my.reg:5000/ns/sshd", "2"}));
}
