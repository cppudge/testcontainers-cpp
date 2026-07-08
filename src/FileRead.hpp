#pragma once

#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <sstream>
#include <string>

#include "testcontainers/Error.hpp"

namespace testcontainers::detail {

/// Read `path` in full (binary); "" if absent/unreadable. For optional config
/// files where a missing file and an empty file mean the same thing.
inline std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Read the whole host file into a string (binary), or throw DockerError with
/// the message prefixed by `context` (e.g. "copy_to_container"). For files the
/// caller named explicitly, where silence would hide a typo.
inline std::string read_file_or_throw(const std::filesystem::path& path,
                                      const std::string& context) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw DockerError(context + ": cannot open host file '" + path.string() + "'");
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw DockerError(context + ": failed reading host file '" + path.string() + "'");
    }
    return data;
}

} // namespace testcontainers::detail
