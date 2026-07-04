#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include "docker/TlsConfig.hpp"

// Tests in this file:
//   TlsConfig.ResolveTlsFilesUsesDockerFixedNames - resolve_tls_files names ca.pem / cert.pem / key.pem inside the cert dir.
//   TlsConfig.ResolveTlsFilesEmptyDirIsAllEmpty - an empty cert dir yields all-empty file paths.
//   TlsConfig.DockerTlsVerifyTruthyValues - DOCKER_TLS_VERIFY in {1,true,TRUE,True} is truthy.
//   TlsConfig.DockerTlsVerifyFalsyValues - empty / unset / "0" / "false" DOCKER_TLS_VERIFY is falsy.
//   TlsConfig.DockerCertPathReturnsEnvWhenSet - docker_cert_path returns DOCKER_CERT_PATH verbatim when set.
//   TlsConfig.DockerCertPathEmptyWhenUnsetAndNoVerify - no DOCKER_CERT_PATH and no verify -> empty.

using testcontainers::docker::docker_cert_path;
using testcontainers::docker::docker_tls_verify;
using testcontainers::docker::resolve_tls_files;
using testcontainers::docker::TlsFiles;

namespace {

// Save/set/restore an environment variable for a test. A nullopt value clears it.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::optional<std::string>& value) : name_(name) {
        if (const char* prev = std::getenv(name)) {
            saved_ = prev;
        }
        apply(value);
    }
    ~ScopedEnv() { apply(saved_); }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void apply(const std::optional<std::string>& value) {
#if defined(_WIN32)
        ::_putenv_s(name_.c_str(), value ? value->c_str() : "");
#else
        if (value) {
            ::setenv(name_.c_str(), value->c_str(), /*overwrite*/ 1);
        } else {
            ::unsetenv(name_.c_str());
        }
#endif
    }

    std::string name_;
    std::optional<std::string> saved_;
};

// The basename of a path, separator-independent (the impl uses the platform's).
std::string basename_of(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

} // namespace

TEST(TlsConfig, ResolveTlsFilesUsesDockerFixedNames) {
    const TlsFiles files = resolve_tls_files("/certs");
    // Assert the basenames (the separator the impl produces is platform-specific).
    EXPECT_EQ(basename_of(files.ca_cert), "ca.pem");
    EXPECT_EQ(basename_of(files.client_cert), "cert.pem");
    EXPECT_EQ(basename_of(files.client_key), "key.pem");
    // And that each path lives under the requested directory.
    EXPECT_NE(files.ca_cert.find("certs"), std::string::npos);
    EXPECT_NE(files.client_cert.find("certs"), std::string::npos);
    EXPECT_NE(files.client_key.find("certs"), std::string::npos);
}

TEST(TlsConfig, ResolveTlsFilesEmptyDirIsAllEmpty) {
    const TlsFiles files = resolve_tls_files("");
    EXPECT_TRUE(files.ca_cert.empty());
    EXPECT_TRUE(files.client_cert.empty());
    EXPECT_TRUE(files.client_key.empty());
}

TEST(TlsConfig, DockerTlsVerifyTruthyValues) {
    for (const char* truthy : {"1", "true", "TRUE", "True"}) {
        ScopedEnv env("DOCKER_TLS_VERIFY", std::string(truthy));
        EXPECT_TRUE(docker_tls_verify()) << "value: " << truthy;
    }
}

TEST(TlsConfig, DockerTlsVerifyFalsyValues) {
    {
        ScopedEnv env("DOCKER_TLS_VERIFY", std::string(""));
        EXPECT_FALSE(docker_tls_verify());
    }
    {
        ScopedEnv env("DOCKER_TLS_VERIFY", std::nullopt); // unset
        EXPECT_FALSE(docker_tls_verify());
    }
    {
        ScopedEnv env("DOCKER_TLS_VERIFY", std::string("0"));
        EXPECT_FALSE(docker_tls_verify());
    }
    {
        ScopedEnv env("DOCKER_TLS_VERIFY", std::string("false"));
        EXPECT_FALSE(docker_tls_verify());
    }
}

TEST(TlsConfig, DockerCertPathReturnsEnvWhenSet) {
    ScopedEnv cert("DOCKER_CERT_PATH", std::string("/my/certs"));
    // Verify on or off, an explicit DOCKER_CERT_PATH wins verbatim.
    ScopedEnv verify("DOCKER_TLS_VERIFY", std::string("1"));
    EXPECT_EQ(docker_cert_path(), "/my/certs");
}

TEST(TlsConfig, DockerCertPathEmptyWhenUnsetAndNoVerify) {
    ScopedEnv cert("DOCKER_CERT_PATH", std::nullopt);    // unset
    ScopedEnv verify("DOCKER_TLS_VERIFY", std::nullopt); // unset -> no ~/.docker fallback
    EXPECT_TRUE(docker_cert_path().empty());
}
