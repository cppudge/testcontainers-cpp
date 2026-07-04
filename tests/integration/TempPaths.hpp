#pragma once

#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <string>
#include <system_error>

// Self-cleaning temp-path helpers shared by the integration tests. Names carry
// a random suffix so two test PROCESSES sharing a temp dir (or a daemon, for
// resource names built from random_suffix()) can never collide — a per-process
// counter cannot give that.
namespace tcit {

/// An 8-hex-char random suffix for file/resource names that must be unique
/// across concurrently running test processes.
inline std::string random_suffix() {
    static constexpr char digits[] = "0123456789abcdef";
    std::random_device rd;
    std::string out;
    for (int word = 0; word < 2; ++word) {
        unsigned value = rd();
        for (int nibble = 0; nibble < 4; ++nibble) {
            out += digits[value & 0xF];
            value >>= 4;
        }
    }
    return out;
}

/// A self-cleaning temp file holding `content`.
class TempFile {
public:
    explicit TempFile(const std::string& content, const std::string& prefix = "tc_file_") {
        path_ = std::filesystem::temp_directory_path() / (prefix + random_suffix());
        std::ofstream(path_, std::ios::binary) << content;
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::string string() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

/// A self-cleaning temp directory holding one file, for bind-mount tests.
class TempDirWithFile {
public:
    TempDirWithFile(const std::string& filename, const std::string& content) {
        dir_ = std::filesystem::temp_directory_path() / ("tc_dir_" + random_suffix());
        std::filesystem::create_directories(dir_);
        std::ofstream(dir_ / filename, std::ios::binary) << content;
    }
    ~TempDirWithFile() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    TempDirWithFile(const TempDirWithFile&) = delete;
    TempDirWithFile& operator=(const TempDirWithFile&) = delete;

    /// Forward-slash form: Docker Desktop takes "C:/Users/..." for a Windows
    /// host path, and on a Linux host it is the path unchanged.
    std::string mount_source() const { return dir_.generic_string(); }

private:
    std::filesystem::path dir_;
};

} // namespace tcit
