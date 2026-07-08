#include "docker/Auth.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

#include "Env.hpp"
#include "FileRead.hpp"
#include "Process.hpp"

namespace testcontainers::docker {

namespace {

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Reverse-lookup table: ASCII char -> 6-bit value, or 0xFF for non-alphabet.
std::array<std::uint8_t, 256> make_decode_table() {
    std::array<std::uint8_t, 256> table{};
    table.fill(0xFF);
    for (std::uint8_t i = 0; i < 64; ++i) {
        table[static_cast<unsigned char>(kBase64Alphabet[i])] = i;
    }
    return table;
}

// The canonical Docker Hub registry host and the legacy alias key Docker uses
// for it inside config.json's "auths" map.
constexpr const char* kDockerHub = "index.docker.io";
constexpr const char* kDockerHubAuthKey = "https://index.docker.io/v1/";

} // namespace

std::string base64_encode(const std::string& bytes) {
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);

    std::size_t i = 0;
    const std::size_t n = bytes.size();
    while (i + 3 <= n) {
        const auto b0 = static_cast<unsigned char>(bytes[i]);
        const auto b1 = static_cast<unsigned char>(bytes[i + 1]);
        const auto b2 = static_cast<unsigned char>(bytes[i + 2]);
        out.push_back(kBase64Alphabet[b0 >> 2]);
        out.push_back(kBase64Alphabet[((b0 & 0x03) << 4) | (b1 >> 4)]);
        out.push_back(kBase64Alphabet[((b1 & 0x0F) << 2) | (b2 >> 6)]);
        out.push_back(kBase64Alphabet[b2 & 0x3F]);
        i += 3;
    }

    if (const std::size_t rem = n - i; rem == 1) {
        const auto b0 = static_cast<unsigned char>(bytes[i]);
        out.push_back(kBase64Alphabet[b0 >> 2]);
        out.push_back(kBase64Alphabet[(b0 & 0x03) << 4]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const auto b0 = static_cast<unsigned char>(bytes[i]);
        const auto b1 = static_cast<unsigned char>(bytes[i + 1]);
        out.push_back(kBase64Alphabet[b0 >> 2]);
        out.push_back(kBase64Alphabet[((b0 & 0x03) << 4) | (b1 >> 4)]);
        out.push_back(kBase64Alphabet[(b1 & 0x0F) << 2]);
        out.push_back('=');
    }

    return out;
}

std::string base64_decode(const std::string& b64) {
    static const std::array<std::uint8_t, 256> table = make_decode_table();

    std::string out;
    out.reserve((b64.size() / 4) * 3);

    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char ch : b64) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            continue; // padding / whitespace
        }
        const std::uint8_t value = table[c];
        if (value == 0xFF) {
            return {}; // malformed input
        }
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::string resolve_registry(const std::string& image) {
    const std::size_t slash = image.find('/');
    if (slash == std::string::npos) {
        // Bare repository like "alpine" — Docker Hub.
        return kDockerHub;
    }

    // Not const: a const local disables the automatic move on `return first`.
    std::string first = image.substr(0, slash);
    // The first segment is a registry host only if it looks like one: contains a
    // '.' (domain), a ':' (port), or is literally "localhost". Otherwise it is a
    // Docker Hub namespace (e.g. "confluentinc/cp-kafka").
    if (first.find('.') != std::string::npos || first.find(':') != std::string::npos ||
        first == "localhost") {
        return first;
    }
    return kDockerHub;
}

std::optional<RegistryAuth> auth_from_docker_config(const std::string& config_json,
                                                    const std::string& registry) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(config_json);
    } catch (const nlohmann::json::parse_error&) {
        return std::nullopt;
    }
    if (!root.is_object()) {
        return std::nullopt;
    }

    const auto auths = root.find("auths");
    if (auths == root.end() || !auths->is_object()) {
        // No plaintext "auths" map — there is nothing for THIS pure lookup to
        // return. A credential helper (credsStore / credHelpers), if configured,
        // is resolved by select_credential_helper / auth_from_credential_helper.
        return std::nullopt;
    }

    // Collect the keys to try: the registry itself, plus the Docker Hub legacy
    // alias when this is Docker Hub.
    auto entry = auths->find(registry);
    if (entry == auths->end() && registry == kDockerHub) {
        entry = auths->find(kDockerHubAuthKey);
    }
    if (entry == auths->end() || !entry->is_object()) {
        return std::nullopt;
    }

    RegistryAuth auth;
    auth.server = registry;

    if (const auto token = entry->find("identitytoken");
        token != entry->end() && token->is_string()) {
        auth.identity_token = token->get<std::string>();
    }

    if (const auto encoded = entry->find("auth"); encoded != entry->end() && encoded->is_string()) {
        const std::string decoded = base64_decode(encoded->get<std::string>());
        if (const std::size_t colon = decoded.find(':'); colon != std::string::npos) {
            auth.username = decoded.substr(0, colon);
            auth.password = decoded.substr(colon + 1);
        }
    }
    // An explicit username/password in the entry overrides the decoded "auth".
    if (const auto user = entry->find("username"); user != entry->end() && user->is_string()) {
        auth.username = user->get<std::string>();
    }
    if (const auto pass = entry->find("password"); pass != entry->end() && pass->is_string()) {
        auth.password = pass->get<std::string>();
    }

    if (auth.identity_token.empty() && auth.username.empty() && auth.password.empty()) {
        return std::nullopt; // entry carried nothing usable (e.g. helper-only)
    }
    return auth;
}

std::optional<std::string> select_credential_helper(const std::string& config_json,
                                                    const std::string& registry) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(config_json);
    } catch (const nlohmann::json::parse_error&) {
        return std::nullopt;
    }
    if (!root.is_object()) {
        return std::nullopt;
    }

    // A per-registry credHelpers entry wins over the global credsStore. For
    // Docker Hub also try the legacy "https://index.docker.io/v1/" key.
    if (const auto helpers = root.find("credHelpers");
        helpers != root.end() && helpers->is_object()) {
        auto entry = helpers->find(registry);
        if (entry == helpers->end() && registry == kDockerHub) {
            entry = helpers->find(kDockerHubAuthKey);
        }
        if (entry != helpers->end() && entry->is_string() && !entry->get<std::string>().empty()) {
            return entry->get<std::string>();
        }
    }

    if (const auto store = root.find("credsStore");
        store != root.end() && store->is_string() && !store->get<std::string>().empty()) {
        return store->get<std::string>();
    }

    return std::nullopt;
}

std::optional<RegistryAuth> parse_credential_helper_output(const std::string& helper_json,
                                                           const std::string& registry) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(helper_json);
    } catch (const nlohmann::json::parse_error&) {
        return std::nullopt;
    }
    if (!root.is_object()) {
        return std::nullopt;
    }

    std::string username;
    std::string secret;
    std::string server_url;
    if (const auto user = root.find("Username"); user != root.end() && user->is_string()) {
        username = user->get<std::string>();
    }
    if (const auto sec = root.find("Secret"); sec != root.end() && sec->is_string()) {
        secret = sec->get<std::string>();
    }
    if (const auto url = root.find("ServerURL"); url != root.end() && url->is_string()) {
        server_url = url->get<std::string>();
    }

    if (username.empty() && secret.empty()) {
        return std::nullopt; // helper returned nothing usable
    }

    RegistryAuth auth;
    auth.server = server_url.empty() ? registry : server_url;
    // A "<token>" username signals that Secret is an identity token, not a
    // password (the documented credential-helper convention).
    if (username == "<token>") {
        auth.identity_token = secret;
    } else {
        auth.username = username;
        auth.password = secret;
    }
    return auth;
}

std::optional<RegistryAuth> auth_from_credential_helper(const std::string& helper,
                                                        const std::string& registry) {
    // The server URL written to the helper's stdin: Docker Hub uses the legacy
    // index URL, everything else uses the registry host verbatim.
    const std::string server_url = (registry == kDockerHub) ? kDockerHubAuthKey : registry;
    try {
        const detail::ProcessResult result = detail::run_process(
            {"docker-credential-" + helper, "get"}, std::nullopt, {}, server_url);
        if (result.exit_code != 0) {
            // Non-zero exit means "credentials not found" (or a missing binary) —
            // not an error, just no creds for this registry.
            return std::nullopt;
        }
        return parse_credential_helper_output(result.output, registry);
    } catch (...) {
        // A failure to start the helper / odd output must never propagate; it
        // simply means we fall back to an anonymous pull.
        return std::nullopt;
    }
}

std::string read_docker_auth_config() {
    if (const char* inline_cfg = std::getenv("DOCKER_AUTH_CONFIG"); inline_cfg && *inline_cfg) {
        return inline_cfg;
    }

    if (const char* cfg_dir = std::getenv("DOCKER_CONFIG"); cfg_dir && *cfg_dir) {
        const std::string contents = detail::read_file(std::string(cfg_dir) + "/config.json");
        return contents.empty() ? "{}" : contents;
    }

    if (const std::string home = detail::home_dir(); !home.empty()) {
        const std::string contents = detail::read_file(home + "/.docker/config.json");
        if (!contents.empty()) {
            return contents;
        }
    }
    return "{}";
}

std::string encode_x_registry_auth(const RegistryAuth& auth) {
    nlohmann::json json = nlohmann::json::object();
    if (!auth.identity_token.empty()) {
        json["identitytoken"] = auth.identity_token;
    } else {
        json["username"] = auth.username;
        json["password"] = auth.password;
    }
    json["serveraddress"] = auth.server;
    return base64_encode(json.dump());
}

std::optional<RegistryAuth> resolve_auth_for_image(const std::string& image) {
    const std::string registry = resolve_registry(image);
    const std::string config = read_docker_auth_config();
    if (auto plain = auth_from_docker_config(config, registry)) {
        return plain; // a plaintext "auths" entry wins
    }
    if (auto helper = select_credential_helper(config, registry)) {
        return auth_from_credential_helper(*helper, registry);
    }
    return std::nullopt;
}

std::string apply_hub_image_prefix(const std::string& image, const std::string& prefix) {
    if (prefix.empty()) {
        return image; // no prefix configured
    }
    // Only Docker Hub images are prefixed; anything already qualified with a
    // registry host (ghcr.io/..., localhost:5000/..., my.reg:5000/...) is left
    // alone, matching testcontainers' PrefixingImageNameSubstitutor.
    if (resolve_registry(image) != kDockerHub) {
        return image;
    }
    // Don't double the prefix if the image already carries it.
    if (image.rfind(prefix, 0) == 0) {
        return image;
    }
    // The prefix is prepended verbatim (it usually already ends with '/'); add a
    // separating '/' only when it is missing.
    if (prefix.back() == '/') {
        return prefix + image;
    }
    return prefix + "/" + image;
}

std::string substitute_image_name(const std::string& image) {
    std::string prefix;
    if (const char* p = std::getenv("TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX"); p && *p) {
        prefix = p;
    }
    return apply_hub_image_prefix(image, prefix);
}

} // namespace testcontainers::docker
