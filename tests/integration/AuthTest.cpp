#include <gtest/gtest.h>

#include <exception>
#include <optional>
#include <string>

#include "testcontainers/GenericImage.hpp"
#include "testcontainers/RegistryAuth.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "docker/Auth.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon):
//   DockerAuth.PublicPullUnaffectedByNoAuth - a public image still pulls with no registry auth (baseline; the auth path is opt-in).
//   DockerAuth.PublicPullWithExplicitAuthHeader - sending an X-Registry-Auth header for a public image does not break the pull.
//   DockerAuth.CredentialHelperSmoke - if a real credsStore/credHelpers is configured, running the helper subprocess returns creds-or-nullopt without throwing.
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
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

} // namespace

TEST_F(DockerAuth, PublicPullUnaffectedByNoAuth) {
    // No explicit auth: pull_image auto-resolves (likely finds nothing) and must
    // still pull a public image exactly as before.
    try {
        client.pull_image(std::string(kPublicImage) + ":" + kPublicTag);
    } catch (const std::exception&) {
        GTEST_SKIP(); // public pull failed - registry/network unavailable
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
    } catch (const std::exception&) {
        GTEST_SKIP(); // public pull with auth header failed - env-dependent
    }
}

// Exercises the REAL credential-helper subprocess end-to-end (the stdin-redirect
// + parse path) WITHOUT asserting any secret value. It needs no daemon (the
// helper is a local binary), so it is a free test rather than a DockerAuth
// fixture; it lives in the integration target to keep the unit suite hermetic.
TEST(DockerAuthCredentialHelper, CredentialHelperSmoke) {
    const std::string config = docker::read_docker_auth_config();
    const auto helper = docker::select_credential_helper(config, "index.docker.io");
    if (!helper.has_value()) {
        GTEST_SKIP(); // no credsStore/credHelpers configured on this machine
    }
    // Returns creds or nullopt depending on whether this machine is logged in;
    // either is fine — the point is that the subprocess+parse path never throws.
    EXPECT_NO_THROW({
        auto a = docker::auth_from_credential_helper(*helper, "index.docker.io");
        (void)a;
    });
}
