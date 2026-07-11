#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>

#include "TempHome.hpp"
#include "TestEnv.hpp"
#include "docker/HostResolve.hpp" // sha256_hex (context store directory names)
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerHost.hpp"

// Tests in this file:
//   DockerHost.ParsesUnixSocket - a unix:// URL parses to the Unix scheme with the socket path and a localhost HTTP host.
//   DockerHost.ParsesNamedPipe - an npipe:// URL parses to the NamedPipe scheme keeping the pipe path.
//   DockerHost.ParsesTcpDefaultPort - a tcp:// URL without a port defaults to port 2375.
//   DockerHost.ParsesTcpExplicitPort - a tcp:// URL with an explicit port keeps that host and port.
//   DockerHost.ParsesHttpsDefaultPort - an https:// URL parses to the Https scheme defaulting to port 2376.
//   DockerHost.ParsesHttpAsTcp - an http:// URL is treated as the Tcp scheme (parse never upgrades; only resolve does).
//   DockerHost.ParsesIpv6Literal - a bracketed IPv6 host with a port parses the host and port correctly.
//   DockerHost.BarePathIsUnix - a bare filesystem path with no scheme is treated as a unix socket.
//   DockerHost.UnsupportedSchemeThrows - an unknown scheme throws DockerError.
//   DockerHost.OutOfRangePortThrows - a port above 65535 throws DockerError instead of silently wrapping.
//   DockerHost.NonNumericPortThrows - a non-numeric port or trailing garbage after the digits throws DockerError.
//   DockerHostFile.ResolveUsesDockerHostEnv - resolve() honors the DOCKER_HOST environment variable (no upgrade with TLS verify off).
//   DockerHostFile.ResolveDefaultsToPlatform - resolve() falls back to the platform default endpoint when nothing else is configured.
//   DockerHostFile.ResolveHonorsDockerHostEnv - resolve() round-trips an explicit DOCKER_HOST URL through to_string().
//   DockerHostFile.ResolveEnvTcpUpgradesUnderTlsVerify - DOCKER_TLS_VERIFY turns a tcp:// / portless DOCKER_HOST into https (2376 default), with no context materials attached.
//   DockerHostFile.ResolveEnvTcpKeepsSchemeWhenVerifyOff - DOCKER_TLS_VERIFY unset or falsy leaves a tcp:// DOCKER_HOST alone.
//   DockerHostFile.ResolvePropertiesHostUpgradesViaPropertiesVerify - docker.host + docker.tls.verify from ~/.testcontainers.properties upgrade a tcp:// host to https.
//   DockerHostFile.ResolveContextSuppliesTlsMaterials - a DOCKER_CONTEXT with a TLS store resolves to https with ca/cert/key attached and verify on.
//   DockerHostFile.ResolveContextSkipTlsVerifyDisablesVerify - SkipTLSVerify:true keeps the materials but turns server verification off.
//   DockerHostFile.ResolveContextCaOnlyStillVerifies - a ca.pem-only store (no client pair) upgrades and verifies.
//   DockerHostFile.ResolveContextWithoutTlsStoreStaysTcp - a context without TLS materials keeps its tcp:// endpoint and attaches nothing.

using namespace testcontainers;

using tctest::set_env;

TEST(DockerHost, ParsesUnixSocket) {
    const auto h = DockerHost::parse("unix:///var/run/docker.sock");
    EXPECT_EQ(h.scheme(), DockerScheme::Unix);
    EXPECT_EQ(h.path(), "/var/run/docker.sock");
    EXPECT_EQ(h.http_host(), "localhost");
}

TEST(DockerHost, ParsesNamedPipe) {
    const auto h = DockerHost::parse("npipe:////./pipe/docker_engine");
    EXPECT_EQ(h.scheme(), DockerScheme::NamedPipe);
    EXPECT_EQ(h.path(), "//./pipe/docker_engine");
    EXPECT_EQ(h.http_host(), "localhost");
}

TEST(DockerHost, ParsesTcpDefaultPort) {
    const auto h = DockerHost::parse("tcp://1.2.3.4");
    EXPECT_EQ(h.scheme(), DockerScheme::Tcp);
    EXPECT_EQ(h.hostname(), "1.2.3.4");
    EXPECT_EQ(h.port(), 2375);
    EXPECT_EQ(h.http_host(), "1.2.3.4");
}

TEST(DockerHost, ParsesTcpExplicitPort) {
    const auto h = DockerHost::parse("tcp://docker.example:2376");
    EXPECT_EQ(h.hostname(), "docker.example");
    EXPECT_EQ(h.port(), 2376);
}

TEST(DockerHost, ParsesHttpsDefaultPort) {
    const auto h = DockerHost::parse("https://10.0.0.5");
    EXPECT_EQ(h.scheme(), DockerScheme::Https);
    EXPECT_EQ(h.port(), 2376);
}

TEST(DockerHost, ParsesHttpAsTcp) {
    const auto h = DockerHost::parse("http://host:1234");
    EXPECT_EQ(h.scheme(), DockerScheme::Tcp);
    EXPECT_EQ(h.port(), 1234);
}

TEST(DockerHost, ParsesIpv6Literal) {
    const auto h = DockerHost::parse("tcp://[::1]:2375");
    EXPECT_EQ(h.hostname(), "::1");
    EXPECT_EQ(h.port(), 2375);
}

TEST(DockerHost, BarePathIsUnix) {
    const auto h = DockerHost::parse("/var/run/docker.sock");
    EXPECT_EQ(h.scheme(), DockerScheme::Unix);
    EXPECT_EQ(h.path(), "/var/run/docker.sock");
}

TEST(DockerHost, UnsupportedSchemeThrows) {
    EXPECT_THROW(DockerHost::parse("ftp://nope"), DockerError);
}

TEST(DockerHost, OutOfRangePortThrows) {
    // Without the range check "99999" would wrap to 34463 via the uint16_t cast.
    EXPECT_THROW(DockerHost::parse("tcp://host:99999"), DockerError);
    EXPECT_THROW(DockerHost::parse("tcp://host:0"), DockerError);
}

TEST(DockerHost, NonNumericPortThrows) {
    EXPECT_THROW(DockerHost::parse("tcp://host:abc"), DockerError);
    // Trailing garbage after digits: stoi would silently accept "2375x" as 2375;
    // the from_chars full-match parse rejects it.
    EXPECT_THROW(DockerHost::parse("tcp://host:2375x"), DockerError);
}

namespace {

// resolve() consults DOCKER_HOST / DOCKER_TLS_VERIFY / DOCKER_CONTEXT, the
// properties file, AND ~/.docker under HOME, so every resolve() test runs on
// the shared temp-HOME fixture with the env pinned per test — a developer's
// real Docker Desktop context or exported DOCKER_TLS_VERIFY must not change
// the outcome.
class DockerHostFile : public tcunit::TempHomeTest {
protected:
    void SetUp() override {
        tcunit::TempHomeTest::SetUp();
        docker_host_.emplace("DOCKER_HOST", std::nullopt);
        tls_verify_.emplace("DOCKER_TLS_VERIFY", std::nullopt);
        cert_path_.emplace("DOCKER_CERT_PATH", std::nullopt);
        docker_context_.emplace("DOCKER_CONTEXT", std::nullopt);
    }

    void TearDown() override {
        docker_context_.reset();
        cert_path_.reset();
        tls_verify_.reset();
        docker_host_.reset();
        tcunit::TempHomeTest::TearDown();
    }

    /// Build ~/.docker/contexts/{meta,tls}/<sha256(name)>/... under the temp
    /// HOME: meta.json verbatim, plus one stub PEM per requested file name.
    void write_context(const std::string& name, const std::string& meta_json,
                       std::initializer_list<const char*> pem_files) {
        const std::string sha = docker::sha256_hex(name);
        const std::filesystem::path meta_dir = dir_ / ".docker" / "contexts" / "meta" / sha;
        std::filesystem::create_directories(meta_dir);
        std::ofstream(meta_dir / "meta.json", std::ios::binary) << meta_json;
        const std::filesystem::path tls_dir =
            dir_ / ".docker" / "contexts" / "tls" / sha / "docker";
        std::filesystem::create_directories(tls_dir);
        for (const char* pem : pem_files) {
            std::ofstream(tls_dir / pem, std::ios::binary) << "stub-pem";
        }
    }

    /// The TLS-store path resolve() should hand to the transport.
    std::string tls_file(const std::string& name, const char* pem) const {
        return (dir_ / ".docker" / "contexts" / "tls" / docker::sha256_hex(name) / "docker" / pem)
            .string();
    }

private:
    std::optional<tctest::ScopedEnv> docker_host_;
    std::optional<tctest::ScopedEnv> tls_verify_;
    std::optional<tctest::ScopedEnv> cert_path_;
    std::optional<tctest::ScopedEnv> docker_context_;
};

} // namespace

TEST_F(DockerHostFile, ResolveUsesDockerHostEnv) {
    set_env("DOCKER_HOST", "tcp://9.9.9.9:2375");
    const auto h = DockerHost::resolve();
    EXPECT_EQ(h.scheme(), DockerScheme::Tcp);
    EXPECT_EQ(h.hostname(), "9.9.9.9");
    set_env("DOCKER_HOST", nullptr);
}

TEST_F(DockerHostFile, ResolveDefaultsToPlatform) {
    const auto h = DockerHost::resolve();
#ifdef _WIN32
    EXPECT_EQ(h.scheme(), DockerScheme::NamedPipe);
#else
    EXPECT_EQ(h.scheme(), DockerScheme::Unix);
#endif
    EXPECT_FALSE(h.tls_materials().has_value());
}

TEST_F(DockerHostFile, ResolveHonorsDockerHostEnv) {
    // Daemon-free: setting DOCKER_HOST (step 1) wins over every other source and
    // round-trips through parse(), regardless of the local Docker config.
    constexpr const char* kUrl = "tcp://10.20.30.40:2375";
    set_env("DOCKER_HOST", kUrl);
    const auto h = DockerHost::resolve();
    EXPECT_EQ(h.to_string(), kUrl);
    EXPECT_EQ(h.scheme(), DockerScheme::Tcp);
    EXPECT_EQ(h.hostname(), "10.20.30.40");
    EXPECT_EQ(h.port(), 2375);
    set_env("DOCKER_HOST", nullptr);
}

TEST_F(DockerHostFile, ResolveEnvTcpUpgradesUnderTlsVerify) {
    const tctest::ScopedEnv verify("DOCKER_TLS_VERIFY", std::string("1"));
    {
        const tctest::ScopedEnv host("DOCKER_HOST", std::string("tcp://envhost:7777"));
        const auto h = DockerHost::resolve();
        EXPECT_EQ(h.scheme(), DockerScheme::Https);
        EXPECT_EQ(h.hostname(), "envhost");
        EXPECT_EQ(h.port(), 7777);
        // Env-driven TLS keeps using the DOCKER_CERT_PATH environment: no
        // context materials ride along.
        EXPECT_FALSE(h.tls_materials().has_value());
    }
    {
        // Portless: the https reparse moves the default 2375 -> 2376 (CLI parity).
        const tctest::ScopedEnv host("DOCKER_HOST", std::string("tcp://envhost"));
        const auto h = DockerHost::resolve();
        EXPECT_EQ(h.scheme(), DockerScheme::Https);
        EXPECT_EQ(h.port(), 2376);
    }
    {
        // https:// stays https; unix:// is never upgraded.
        const tctest::ScopedEnv host("DOCKER_HOST", std::string("unix:///var/run/docker.sock"));
        EXPECT_EQ(DockerHost::resolve().scheme(), DockerScheme::Unix);
    }
}

TEST_F(DockerHostFile, ResolveEnvTcpKeepsSchemeWhenVerifyOff) {
    const tctest::ScopedEnv host("DOCKER_HOST", std::string("tcp://envhost:2375"));
    {
        const auto h = DockerHost::resolve(); // DOCKER_TLS_VERIFY unset (fixture)
        EXPECT_EQ(h.scheme(), DockerScheme::Tcp);
    }
    {
        const tctest::ScopedEnv verify("DOCKER_TLS_VERIFY", std::string("0"));
        EXPECT_EQ(DockerHost::resolve().scheme(), DockerScheme::Tcp);
    }
}

TEST_F(DockerHostFile, ResolvePropertiesHostUpgradesViaPropertiesVerify) {
    set_properties("docker.host=tcp://props-host:1234\n"
                   "docker.tls.verify=true\n");
    const auto h = DockerHost::resolve();
    EXPECT_EQ(h.scheme(), DockerScheme::Https);
    EXPECT_EQ(h.hostname(), "props-host");
    EXPECT_EQ(h.port(), 1234);
    EXPECT_FALSE(h.tls_materials().has_value());
}

TEST_F(DockerHostFile, ResolveContextSuppliesTlsMaterials) {
    const std::string name = "tcunit-tls-ctx";
    write_context(
        name, R"({"Name":"tcunit-tls-ctx","Endpoints":{"docker":{"Host":"tcp://ctx-host:7777"}}})",
        {"ca.pem", "cert.pem", "key.pem"});
    const tctest::ScopedEnv ctx("DOCKER_CONTEXT", name);

    const auto h = DockerHost::resolve();
    EXPECT_EQ(h.scheme(), DockerScheme::Https); // materials mean TLS, like the CLI
    EXPECT_EQ(h.hostname(), "ctx-host");
    EXPECT_EQ(h.port(), 7777);
    ASSERT_TRUE(h.tls_materials().has_value());
    EXPECT_EQ(h.tls_materials()->ca_cert, tls_file(name, "ca.pem"));
    EXPECT_EQ(h.tls_materials()->client_cert, tls_file(name, "cert.pem"));
    EXPECT_EQ(h.tls_materials()->client_key, tls_file(name, "key.pem"));
    EXPECT_TRUE(h.tls_materials()->verify);
}

TEST_F(DockerHostFile, ResolveContextSkipTlsVerifyDisablesVerify) {
    const std::string name = "tcunit-skip-ctx";
    write_context(
        name,
        R"({"Name":"tcunit-skip-ctx","Endpoints":{"docker":{"Host":"tcp://ctx-host:7777","SkipTLSVerify":true}}})",
        {"ca.pem", "cert.pem", "key.pem"});
    const tctest::ScopedEnv ctx("DOCKER_CONTEXT", name);

    const auto h = DockerHost::resolve();
    EXPECT_EQ(h.scheme(), DockerScheme::Https);
    ASSERT_TRUE(h.tls_materials().has_value());
    EXPECT_FALSE(h.tls_materials()->verify); // materials still presented, server unchecked
    EXPECT_EQ(h.tls_materials()->client_cert, tls_file(name, "cert.pem"));
}

TEST_F(DockerHostFile, ResolveContextCaOnlyStillVerifies) {
    const std::string name = "tcunit-ca-ctx";
    write_context(
        name, R"({"Name":"tcunit-ca-ctx","Endpoints":{"docker":{"Host":"tcp://ctx-host:7777"}}})",
        {"ca.pem"});
    const tctest::ScopedEnv ctx("DOCKER_CONTEXT", name);

    const auto h = DockerHost::resolve();
    EXPECT_EQ(h.scheme(), DockerScheme::Https);
    ASSERT_TRUE(h.tls_materials().has_value());
    EXPECT_TRUE(h.tls_materials()->verify);
    EXPECT_EQ(h.tls_materials()->ca_cert, tls_file(name, "ca.pem"));
    EXPECT_TRUE(h.tls_materials()->client_cert.empty());
    EXPECT_TRUE(h.tls_materials()->client_key.empty());
}

TEST_F(DockerHostFile, ResolveContextWithoutTlsStoreStaysTcp) {
    const std::string name = "tcunit-plain-ctx";
    write_context(
        name,
        R"({"Name":"tcunit-plain-ctx","Endpoints":{"docker":{"Host":"tcp://ctx-host:2375"}}})", {});
    const tctest::ScopedEnv ctx("DOCKER_CONTEXT", name);

    const auto h = DockerHost::resolve();
    EXPECT_EQ(h.scheme(), DockerScheme::Tcp);
    EXPECT_EQ(h.hostname(), "ctx-host");
    EXPECT_FALSE(h.tls_materials().has_value());
}
