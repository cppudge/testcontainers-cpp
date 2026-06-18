#include <gtest/gtest.h>

#include <cstdlib>

#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerHost.hpp"

// Tests in this file:
//   DockerHost.ParsesUnixSocket - a unix:// URL parses to the Unix scheme with the socket path and a localhost HTTP host.
//   DockerHost.ParsesNamedPipe - an npipe:// URL parses to the NamedPipe scheme keeping the pipe path.
//   DockerHost.ParsesTcpDefaultPort - a tcp:// URL without a port defaults to port 2375.
//   DockerHost.ParsesTcpExplicitPort - a tcp:// URL with an explicit port keeps that host and port.
//   DockerHost.ParsesHttpsDefaultPort - an https:// URL parses to the Https scheme defaulting to port 2376.
//   DockerHost.ParsesHttpAsTcp - an http:// URL is treated as the Tcp scheme.
//   DockerHost.ParsesIpv6Literal - a bracketed IPv6 host with a port parses the host and port correctly.
//   DockerHost.BarePathIsUnix - a bare filesystem path with no scheme is treated as a unix socket.
//   DockerHost.UnsupportedSchemeThrows - an unknown scheme throws DockerError.
//   DockerHost.ResolveUsesDockerHostEnv - resolve() honors the DOCKER_HOST environment variable.
//   DockerHost.ResolveDefaultsToPlatform - resolve() falls back to the platform default endpoint when DOCKER_HOST is unset.

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
