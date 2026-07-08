#pragma once

#include <cstddef>
#include <random>
#include <string>

namespace testcontainers::detail {

/// Generate `chars` random lowercase-hex characters (4 bits of entropy each).
/// The one shared generator behind the session id (32), generated resource
/// names (`tc-<hex>` networks / volumes / compose projects), and temp-file
/// names — previously copy-pasted per translation unit.
inline std::string random_hex(std::size_t chars) {
    static constexpr char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    std::string out;
    out.reserve(chars);
    for (std::size_t i = 0; i < chars; ++i) {
        out.push_back(hex[dist(rd)]);
    }
    return out;
}

/// Generate a unique-ish resource name like "tc-1a2b3c4d5e6f7a8b" — the one
/// shape shared by generated network and volume names.
inline std::string random_resource_name() { return "tc-" + random_hex(16); }

} // namespace testcontainers::detail
