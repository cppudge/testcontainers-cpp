#include <gtest/gtest.h>

#include <cstdlib>

#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerHost.hpp"

using namespace testcontainers;

namespace {
void set_env(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value ? value : ""); // empty value removes it
#else
    if (value) {
        setenv(key, value, 1);
    } else {
        unsetenv(key);
    }
#endif
}
} // namespace

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

TEST(DockerHost, ResolveUsesDockerHostEnv) {
    set_env("DOCKER_HOST", "tcp://9.9.9.9:2375");
    const auto h = DockerHost::resolve();
    EXPECT_EQ(h.scheme(), DockerScheme::Tcp);
    EXPECT_EQ(h.hostname(), "9.9.9.9");
    set_env("DOCKER_HOST", nullptr);
}

TEST(DockerHost, ResolveDefaultsToPlatform) {
    set_env("DOCKER_HOST", nullptr);
    const auto h = DockerHost::resolve();
#ifdef _WIN32
    EXPECT_EQ(h.scheme(), DockerScheme::NamedPipe);
#else
    EXPECT_EQ(h.scheme(), DockerScheme::Unix);
#endif
}
