#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

namespace tcit {

/// The hex capability mask after `label` (e.g. "CapEff:") in /proc/*/status
/// output. Returns 0 when the label is missing OR the value fails to parse
/// (e.g. a truncated log chunk), so the caller's ASSERT against 0 yields a
/// readable diagnostic instead of an unhandled std::invalid_argument abort.
inline std::uint64_t cap_mask_after(const std::string& status_output, const std::string& label) {
    const std::size_t at = status_output.find(label);
    if (at == std::string::npos) {
        return 0;
    }
    try {
        return std::stoull(status_output.substr(at + label.size()), nullptr, 16);
    } catch (const std::exception&) {
        return 0;
    }
}

} // namespace tcit
