#include "Reuse.hpp"

#include "Config.hpp"

#include <cstdint>
#include <string>

namespace testcontainers::detail {

namespace {

constexpr const char* kReuseHashLabel = "org.testcontainers.reuse.hash";

} // namespace

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
    return config_truthy("TESTCONTAINERS_REUSE_ENABLE", "testcontainers.reuse.enable");
}

} // namespace testcontainers::detail
