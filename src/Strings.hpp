#pragma once

#include <cctype>
#include <cstddef>
#include <string>

namespace testcontainers::detail {

/// Trim ASCII spaces / tabs / CR / LF from both ends of `s`; "" if all
/// whitespace. A fixed four-character set, not locale whitespace — used for
/// Java-properties-style line parsing where only these four may surround
/// tokens.
inline std::string trim(const std::string& s) {
    const std::size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

/// ASCII-lowercase a copy of `s`.
inline std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

} // namespace testcontainers::detail
