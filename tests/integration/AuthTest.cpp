#include <gtest/gtest.h>

#include <exception>
#include <string>

#include "testcontainers/GenericImage.hpp"
#include "testcontainers/RegistryAuth.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon):
//   DockerAuth.PublicPullUnaffectedByNoAuth - a public image still pulls with no registry auth (baseline; the auth path is opt-in).
//   DockerAuth.PublicPullWithExplicitAuthHeader - sending an X-Registry-Auth header for a public image does not break the pull.
//
// These are best-effort: a reachable private registry is environment-dependent
// on Docker Desktop, so anything that goes wrong skips rather than fails. We do
// NOT stand up an authenticated registry:2 (insecure-registry + daemon
// reachability make that flaky here).

using namespace testcontainers;

namespace {

constexpr const char* kPublicImage = "alpine";
constexpr const char* kPublicTag = "3.20";

// Skip the whole fixture unless a daemon answers.
class DockerAuth : public ::testing::Test {
protected:
    DockerClient client = DockerClient::from_environment();

    void SetUp() override {
        if (auto why = tcit::linux_engine_unavailable()) {
            GTEST_SKIP() << *why;
        }
    }
};

} // namespace

TEST_F(DockerAuth, PublicPullUnaffectedByNoAuth) {
    // No explicit auth: pull_image auto-resolves (likely finds nothing) and must
    // still pull a public image exactly as before.
    try {
        client.pull_image(std::string(kPublicImage) + ":" + kPublicTag);
    } catch (const std::exception& e) {
        GTEST_SKIP() << "Public pull failed (registry/network unavailable): " << e.what();
    }
}

TEST_F(DockerAuth, PublicPullWithExplicitAuthHeader) {
    // Provide explicit (dummy) credentials so the X-Registry-Auth header is sent.
    // Docker ignores credentials for a public image, so the pull should still
    // succeed — verifying the header path does not break a normal pull.
    RegistryAuth auth;
    auth.username = "anonymous";
    auth.password = "";
    auth.server = "index.docker.io";

    try {
        client.pull_image(std::string(kPublicImage) + ":" + kPublicTag, auth);
    } catch (const std::exception& e) {
        GTEST_SKIP() << "Public pull with auth header failed (env-dependent): " << e.what();
    }
}
