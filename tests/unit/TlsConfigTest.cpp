#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>

#include "TempHome.hpp"
#include "TestEnv.hpp"
#include "docker/TlsConfig.hpp"

// Tests in this file:
//   TlsConfig.ResolveTlsFilesUsesDockerFixedNames - resolve_tls_files names ca.pem / cert.pem / key.pem inside the cert dir.
//   TlsConfig.ResolveTlsFilesEmptyDirIsAllEmpty - an empty cert dir yields all-empty file paths.
//   TlsConfig.DockerTlsVerifyTruthyValues - DOCKER_TLS_VERIFY in {1,true,TRUE,True} is truthy.
//   TlsConfigFile.DockerTlsVerifyFalsyValues - empty / unset / "0" / "false" DOCKER_TLS_VERIFY is falsy (under a temp HOME: unset/empty now fall through to the properties file).
//   TlsConfig.DockerCertPathReturnsEnvWhenSet - docker_cert_path returns DOCKER_CERT_PATH verbatim when set.
//   TlsConfigFile.DockerCertPathEmptyWhenUnsetAndNoVerify - no DOCKER_CERT_PATH, no properties key, and no verify -> empty.
//   TlsConfigFile.TlsVerifyPropertiesAcceptsOneAndTrue - without the env var, docker.tls.verify decides: "1" and case-insensitive "true" are truthy (docker-java parity), "0" is not, and a set env var still wins.
//   TlsConfigFile.CertPathPropertiesFallback - without DOCKER_CERT_PATH the docker.cert.path key supplies the dir; the env var beats it.
//   TlsConfig.TlsPlanUsesMaterialsVerbatim - context materials become the plan verbatim (paths + verify), no env consulted.
//   TlsConfigFile.TlsPlanFallsBackToEnvironment - without materials the plan derives from DOCKER_CERT_PATH/DOCKER_TLS_VERIFY (docker's fixed file names); with nothing set it is empty and unverified.

using testcontainers::TlsMaterials;
using testcontainers::docker::docker_cert_path;
using testcontainers::docker::docker_tls_verify;
using testcontainers::docker::resolve_tls_files;
using testcontainers::docker::tls_plan;
using testcontainers::docker::TlsFiles;
using testcontainers::docker::TlsPlan;

namespace {

using tctest::ScopedEnv;

// The basename of a path, separator-independent (the impl uses the platform's).
std::string basename_of(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

} // namespace

// Everything whose env var is unset/empty falls through to the properties
// file, so those tests run on the shared temp-HOME fixture — a developer's
// real ~/.testcontainers.properties (docker.tls.verify / docker.cert.path)
// must not bleed into them.
using TlsConfigFile = tcunit::TempHomeTest;

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

TEST_F(TlsConfigFile, DockerTlsVerifyFalsyValues) {
    {
        ScopedEnv env("DOCKER_TLS_VERIFY", std::string(""));
        EXPECT_FALSE(docker_tls_verify());
    }
    {
        ScopedEnv env("DOCKER_TLS_VERIFY", std::nullopt); // unset -> properties (absent here)
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

TEST_F(TlsConfigFile, DockerCertPathEmptyWhenUnsetAndNoVerify) {
    ScopedEnv cert("DOCKER_CERT_PATH", std::nullopt);    // unset -> properties (absent here)
    ScopedEnv verify("DOCKER_TLS_VERIFY", std::nullopt); // unset + no properties -> verify off,
                                                         // so no ~/.docker fallback either
    EXPECT_TRUE(docker_cert_path().empty());
}

TEST_F(TlsConfigFile, TlsVerifyPropertiesAcceptsOneAndTrue) {
    const ScopedEnv verify("DOCKER_TLS_VERIFY", std::nullopt); // unset -> properties decide

    // docker-java parses this shared key as "1" OR Boolean.parseBoolean, so
    // BOTH spellings must verify here (unlike the parseBoolean-only
    // testcontainers keys, where "1" is false).
    for (const char* on : {"1", "true", "TRUE"}) {
        set_properties(std::string("docker.tls.verify=") + on + "\n");
        EXPECT_TRUE(docker_tls_verify()) << on;
    }
    set_properties("docker.tls.verify=0\n");
    EXPECT_FALSE(docker_tls_verify());

    // A set env var decides over a file-enabled switch.
    set_properties("docker.tls.verify=true\n");
    const ScopedEnv env_off("DOCKER_TLS_VERIFY", std::string("false"));
    EXPECT_FALSE(docker_tls_verify());
}

TEST_F(TlsConfigFile, CertPathPropertiesFallback) {
    set_properties("docker.cert.path=/props/certs\n");
    {
        const ScopedEnv cert("DOCKER_CERT_PATH", std::nullopt); // unset -> properties
        EXPECT_EQ(docker_cert_path(), "/props/certs");
    }
    {
        const ScopedEnv cert("DOCKER_CERT_PATH", std::string("/env/certs"));
        EXPECT_EQ(docker_cert_path(), "/env/certs");
    }
}

TEST(TlsConfig, TlsPlanUsesMaterialsVerbatim) {
    TlsMaterials materials;
    materials.ca_cert = "/ctx/ca.pem";
    materials.client_cert = "/ctx/cert.pem";
    materials.client_key = "/ctx/key.pem";
    materials.verify = true;

    const TlsPlan plan = tls_plan(materials);
    EXPECT_EQ(plan.ca_cert, "/ctx/ca.pem");
    EXPECT_EQ(plan.client_cert, "/ctx/cert.pem");
    EXPECT_EQ(plan.client_key, "/ctx/key.pem");
    EXPECT_TRUE(plan.verify);

    materials.verify = false;
    EXPECT_FALSE(tls_plan(materials).verify);
}

TEST_F(TlsConfigFile, TlsPlanFallsBackToEnvironment) {
    {
        const ScopedEnv cert("DOCKER_CERT_PATH", std::string("/env/certs"));
        const ScopedEnv verify("DOCKER_TLS_VERIFY", std::string("1"));
        const TlsPlan plan = tls_plan(std::nullopt);
        EXPECT_EQ(basename_of(plan.ca_cert), "ca.pem");
        EXPECT_EQ(basename_of(plan.client_cert), "cert.pem");
        EXPECT_EQ(basename_of(plan.client_key), "key.pem");
        EXPECT_NE(plan.ca_cert.find("certs"), std::string::npos);
        EXPECT_TRUE(plan.verify);
    }
    {
        const ScopedEnv cert("DOCKER_CERT_PATH", std::nullopt);
        const ScopedEnv verify("DOCKER_TLS_VERIFY", std::nullopt);
        const TlsPlan plan = tls_plan(std::nullopt); // nothing anywhere (temp HOME)
        EXPECT_TRUE(plan.ca_cert.empty());
        EXPECT_TRUE(plan.client_cert.empty());
        EXPECT_TRUE(plan.client_key.empty());
        EXPECT_FALSE(plan.verify);
    }
}
