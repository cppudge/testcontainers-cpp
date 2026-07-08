#include "testcontainers/docker/DockerHost.hpp"

#include "Env.hpp"
#include "FileRead.hpp"
#include "Strings.hpp"
#include "docker/HostResolve.hpp"

#include "testcontainers/Error.hpp"

#include <bit>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#include <nlohmann/json.hpp>

namespace testcontainers {

namespace {

// Parse "host:port" (optionally followed by a path) into the host fields.
void parse_host_port(std::string rest, std::string& hostname, std::uint16_t& port,
                     std::uint16_t default_port) {
    if (const auto slash = rest.find('/'); slash != std::string::npos) {
        rest = rest.substr(0, slash); // drop any trailing path
    }

    std::string host;
    std::string port_str;
    if (!rest.empty() && rest.front() == '[') {
        // IPv6 literal: [::1]:2375
        const auto close = rest.find(']');
        host = rest.substr(1, close == std::string::npos ? std::string::npos : close - 1);
        if (close != std::string::npos && close + 1 < rest.size() && rest[close + 1] == ':') {
            port_str = rest.substr(close + 2);
        }
    } else if (const auto colon = rest.rfind(':'); colon != std::string::npos) {
        host = rest.substr(0, colon);
        port_str = rest.substr(colon + 1);
    } else {
        host = rest;
    }

    hostname = host;
    port = default_port;
    if (!port_str.empty()) {
        // from_chars with a full-match check: rejects "2375x" / whitespace /
        // "+2375" (which stoi silently accepts) and "99999"/"0" (out of range —
        // without the check "99999" would wrap to 34463 via the uint16_t cast).
        std::uint32_t parsed = 0;
        const auto [end, err] =
            std::from_chars(port_str.data(), port_str.data() + port_str.size(), parsed);
        if (err != std::errc{} || end != port_str.data() + port_str.size() || parsed < 1 ||
            parsed > 65535) {
            throw DockerError("Invalid port in Docker host: " + port_str);
        }
        port = static_cast<std::uint16_t>(parsed);
    }
}

} // namespace

namespace docker {

namespace {

// ===== Self-contained SHA-256 (public-domain style; no OpenSSL) =====
// Only used to name the docker-context meta directory. Operates on bytes.

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

} // namespace

std::string sha256_hex(const std::string& data) {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    // Pad: append 0x80, then zeros, then the 64-bit big-endian bit length.
    std::string msg = data;
    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) {
        msg.push_back('\0');
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xFF));
    }

    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const std::size_t base = off + static_cast<std::size_t>(i) * 4;
            const auto b0 = static_cast<unsigned char>(msg[base + 0]);
            const auto b1 = static_cast<unsigned char>(msg[base + 1]);
            const auto b2 = static_cast<unsigned char>(msg[base + 2]);
            const auto b3 = static_cast<unsigned char>(msg[base + 3]);
            w[i] = (static_cast<std::uint32_t>(b0) << 24) | (static_cast<std::uint32_t>(b1) << 16) |
                   (static_cast<std::uint32_t>(b2) << 8) | static_cast<std::uint32_t>(b3);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 =
                std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
            const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    static constexpr char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            out[i * 8 + j] = hex[(h[i] >> ((7 - j) * 4)) & 0xF];
        }
    }
    return out;
}

std::optional<std::string> docker_host_from_properties(const std::string& properties_body) {
    using detail::trim;

    std::istringstream stream(properties_body);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == '!') {
            continue; // blank or comment line ('#' and '!' both start comments)
        }
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        if (trim(trimmed.substr(0, eq)) == "docker.host") {
            // Not const: a const local disables the automatic move into the
            // returned optional.
            std::string value = trim(trimmed.substr(eq + 1));
            if (!value.empty()) {
                return value;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> current_context_from_config(const std::string& config_json) {
    try {
        const nlohmann::json root = nlohmann::json::parse(config_json);
        if (!root.is_object()) {
            return std::nullopt;
        }
        if (const auto ctx = root.find("currentContext"); ctx != root.end() && ctx->is_string()) {
            return ctx->get<std::string>();
        }
    } catch (const nlohmann::json::parse_error&) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> docker_host_from_context_meta(const std::string& meta_json) {
    try {
        const nlohmann::json root = nlohmann::json::parse(meta_json);
        if (!root.is_object()) {
            return std::nullopt;
        }
        // Endpoints.docker.Host
        const auto endpoints = root.find("Endpoints");
        if (endpoints == root.end() || !endpoints->is_object()) {
            return std::nullopt;
        }
        const auto dockerep = endpoints->find("docker");
        if (dockerep == endpoints->end() || !dockerep->is_object()) {
            return std::nullopt;
        }
        if (const auto host = dockerep->find("Host");
            host != dockerep->end() && host->is_string()) {
            // Not const: allows the automatic move into the returned optional.
            std::string value = host->get<std::string>();
            if (!value.empty()) {
                return value;
            }
        }
    } catch (const nlohmann::json::parse_error&) {
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace docker

DockerHost DockerHost::parse(const std::string& url) {
    DockerHost h;
    h.url_ = url;

    std::string scheme;
    std::string rest;
    if (const auto sep = url.find("://"); sep != std::string::npos) {
        scheme = detail::to_lower(url.substr(0, sep));
        rest = url.substr(sep + 3);
    } else if (!url.empty() && (url.front() == '/' || url.front() == '\\')) {
        scheme = "unix"; // bare path
        rest = url;
    } else {
        scheme = "tcp"; // bare host:port
        rest = url;
    }

    if (scheme == "unix") {
        h.scheme_ = DockerScheme::Unix;
        h.path_ = rest;
    } else if (scheme == "npipe") {
        h.scheme_ = DockerScheme::NamedPipe;
        h.path_ = rest;
    } else if (scheme == "https") {
        h.scheme_ = DockerScheme::Https;
        parse_host_port(rest, h.hostname_, h.port_, 2376);
    } else if (scheme == "tcp" || scheme == "http") {
        h.scheme_ = DockerScheme::Tcp;
        parse_host_port(rest, h.hostname_, h.port_, 2375);
    } else {
        throw DockerError("Unsupported Docker host scheme: '" + scheme + "' in " + url);
    }

    return h;
}

namespace {

// Step 2: docker.host from ~/.testcontainers.properties. nullopt on
// absent/unreadable/missing-key (never throws).
std::optional<DockerHost> resolve_from_properties() {
    const std::string home = detail::home_dir();
    if (home.empty()) {
        return std::nullopt;
    }
    const std::string body = detail::read_file(home + "/.testcontainers.properties");
    if (body.empty()) {
        return std::nullopt;
    }
    const auto host = docker::docker_host_from_properties(body);
    if (!host) {
        return std::nullopt;
    }
    try {
        return DockerHost::parse(*host);
    } catch (const DockerError&) {
        return std::nullopt; // malformed value falls through to the next step
    }
}

// Step 3: the active docker context's endpoint. The context name is
// DOCKER_CONTEXT, else "currentContext" in ~/.docker/config.json, else
// "default". An empty / "default" name resolves nothing (use step 4). nullopt
// on any missing/unreadable file (never throws).
std::optional<DockerHost> resolve_from_context() {
    const std::string home = detail::home_dir();

    std::string context;
    if (const char* env = std::getenv("DOCKER_CONTEXT"); env && *env) {
        context = env;
    } else if (!home.empty()) {
        const std::string config = detail::read_file(home + "/.docker/config.json");
        if (!config.empty()) {
            if (const auto current = docker::current_context_from_config(config)) {
                context = *current;
            }
        }
    }

    if (context.empty() || context == "default") {
        return std::nullopt; // no explicit context — fall through to the default socket
    }
    if (home.empty()) {
        return std::nullopt; // can't locate the meta directory without a home
    }

    const std::string meta_path =
        home + "/.docker/contexts/meta/" + docker::sha256_hex(context) + "/meta.json";
    const std::string meta = detail::read_file(meta_path);
    if (meta.empty()) {
        return std::nullopt;
    }
    const auto host = docker::docker_host_from_context_meta(meta);
    if (!host) {
        return std::nullopt;
    }
    try {
        return DockerHost::parse(*host);
    } catch (const DockerError&) {
        return std::nullopt; // malformed endpoint falls through to the next step
    }
}

} // namespace

DockerHost DockerHost::resolve() {
    // 1. DOCKER_HOST env var (a malformed value here still throws via parse).
    if (const char* docker_host = std::getenv("DOCKER_HOST"); docker_host && *docker_host) {
        return parse(docker_host);
    }

    // 2. docker.host from ~/.testcontainers.properties.
    if (auto host = resolve_from_properties()) {
        return *host;
    }

    // 3. The active Docker context's endpoint.
    if (auto host = resolve_from_context()) {
        return *host;
    }

    // 4. Platform default, with rootless socket fallbacks (non-Windows).
#if defined(_WIN32)
    return parse("npipe:////./pipe/docker_engine");
#else
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && *xdg) {
        const std::string sock = std::string(xdg) + "/docker.sock";
        if (std::filesystem::exists(sock)) {
            return parse("unix://" + sock);
        }
    }
    if (const std::string home = detail::home_dir(); !home.empty()) {
        const std::string sock = home + "/.docker/run/docker.sock";
        if (std::filesystem::exists(sock)) {
            return parse("unix://" + sock);
        }
    }
    return parse("unix:///var/run/docker.sock");
#endif
}

std::string DockerHost::http_host() const {
    switch (scheme_) {
    case DockerScheme::Unix:
    case DockerScheme::NamedPipe:
        return "localhost";
    case DockerScheme::Tcp:
    case DockerScheme::Https:
        return hostname_.empty() ? "localhost" : hostname_;
    }
    return "localhost";
}

} // namespace testcontainers
