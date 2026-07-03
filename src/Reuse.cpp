#define _CRT_SECURE_NO_WARNINGS // std::getenv on MSVC

#include "Reuse.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace testcontainers::detail {

namespace {

constexpr const char* kReuseHashLabel = "org.testcontainers.reuse.hash";

/// Trim ASCII spaces / tabs / CR / LF from both ends of `s`.
std::string trim(const std::string& s) {
    const std::size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

bool env_truthy(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr) {
        return false;
    }
    const std::string s(v);
    return s == "1" || s == "true" || s == "TRUE" || s == "True";
}

/// Read ~/.testcontainers.properties (HOME or USERPROFILE) in full; "" if absent.
std::string read_properties_file() {
    std::string home;
    if (const char* h = std::getenv("HOME"); h && *h) {
        home = h;
    } else if (const char* up = std::getenv("USERPROFILE"); up && *up) {
        home = up; // Windows
    }
    if (home.empty()) {
        return {};
    }
    std::ifstream in(home + "/.testcontainers.properties", std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// ASCII-lowercase a copy of `s`.
std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

} // namespace

/// See Reuse.hpp.
bool properties_reuse_enabled(const std::string& contents) {
    std::istringstream stream(contents);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue; // blank or comment line
        }
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        // The value is compared case-insensitively ("true"/"TRUE"/"True"):
        // ~/.testcontainers.properties is the same file testcontainers-java
        // reads, and java parses it with the case-insensitive
        // Boolean.parseBoolean — a value that enables reuse there must not
        // silently disable it here.
        if (trim(trimmed.substr(0, eq)) == "testcontainers.reuse.enable" &&
            to_lower(trim(trimmed.substr(eq + 1))) == "true") {
            return true;
        }
    }
    return false;
}

const char* reuse_hash_label() { return kReuseHashLabel; }

std::string reuse_hash(const std::string& canonical) {
    // FNV-1a, 64-bit. Deterministic across runs/platforms; not cryptographic.
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;

    std::uint64_t hash = kOffsetBasis;
    for (const char ch : canonical) {
        hash ^= static_cast<std::uint8_t>(ch);
        hash *= kPrime;
    }

    // Render as 16 lowercase hex chars, zero-padded.
    static constexpr char hex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[i] = hex[hash & 0xF];
        hash >>= 4;
    }
    return out;
}

bool reuse_enabled() {
    if (env_truthy("TESTCONTAINERS_REUSE_ENABLE")) {
        return true;
    }
    return properties_reuse_enabled(read_properties_file());
}

} // namespace testcontainers::detail
