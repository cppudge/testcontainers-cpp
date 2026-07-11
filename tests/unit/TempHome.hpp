#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include "Config.hpp"
#include "TestEnv.hpp"

namespace tcunit {

/// Base fixture for tests exercising anything that falls through to
/// ~/.testcontainers.properties: points HOME/USERPROFILE at a fresh temp dir
/// and clears the process-global properties cache on BOTH ends, so tests
/// neither see a developer's real properties file nor leak their temp file
/// into later tests through the cache. Alias it per file to keep a meaningful
/// suite name: `using ConfigFile = tcunit::TempHomeTest;`.
class TempHomeTest : public ::testing::Test {
protected:
    void SetUp() override {
        static std::atomic<unsigned> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("tc_temphome_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(dir_);
        home_.emplace("HOME", dir_.string());
        userprofile_.emplace("USERPROFILE", dir_.string());
        testcontainers::detail::clear_user_properties_cache();
    }

    void TearDown() override {
        testcontainers::detail::clear_user_properties_cache();
        home_.reset();
        userprofile_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// (Over)write the temp HOME's .testcontainers.properties with `body`
    /// WITHOUT touching the cache (for pinning the cache behavior itself).
    void write_properties(const std::string& body) {
        std::ofstream out(dir_ / ".testcontainers.properties", std::ios::binary);
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }

    /// write_properties + cache clear: the next config read sees exactly `body`.
    void set_properties(const std::string& body) {
        write_properties(body);
        testcontainers::detail::clear_user_properties_cache();
    }

    std::filesystem::path dir_;

private:
    std::optional<tctest::ScopedEnv> home_;
    std::optional<tctest::ScopedEnv> userprofile_;
};

} // namespace tcunit
