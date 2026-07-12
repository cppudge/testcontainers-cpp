#include <gtest/gtest.h>

#include <string>

#include "testcontainers/ExecResult.hpp"
#include "testcontainers/Network.hpp"
#include "testcontainers/modules/MinIO.hpp"
#include "testcontainers/modules/RustFS.hpp"

#include "EngineGuard.hpp"
#include "HttpGet.hpp"

// Tests in this file (integration; require a Linux-containers Docker daemon):
//   RustFSModule.DefaultsStartHealthAndAuthLayer - a default RustFSImage starts, the getters report the rustfsadmin pair and the URL shapes, /health answers 200 with a ready report through the published port, and an anonymous S3 request is denied (403 — the auth layer is up, which /health alone cannot prove).
//   RustFSModule.CredentialsEnforcedAcrossNetwork - custom RUSTFS_* credentials are consumed by the server (the beta line once regressed here): a sibling MinIO container's mc, aliased across a user-defined network, authenticates with them, creates a bucket, and is rejected with a wrong secret.
//   RustFSModule.ConsoleServesUnderPrefix - the published console port serves the web UI under /rustfs/console/ (the bare root answers 403 by design).

using namespace testcontainers;
using modules::MinIOContainer;
using modules::MinIOImage;
using modules::RustFSContainer;
using modules::RustFSImage;

// Requires a Linux-containers daemon; skipped otherwise.
class RustFSModule : public tcit::LinuxEngineTest {};

TEST_F(RustFSModule, DefaultsStartHealthAndAuthLayer) {
    const RustFSContainer rustfs = RustFSImage().start();

    EXPECT_EQ(rustfs.access_key(), "rustfsadmin");
    EXPECT_EQ(rustfs.secret_key(), "rustfsadmin");
    EXPECT_EQ(rustfs.s3_url(), "http://" + rustfs.host() + ":" + std::to_string(rustfs.s3_port()));
    EXPECT_EQ(rustfs.console_url(), "http://" + rustfs.host() + ":" +
                                        std::to_string(rustfs.console_port()) + "/rustfs/console/");

    const std::string health = tcit::http_get(rustfs.host(), rustfs.s3_port(), "/health");
    EXPECT_EQ(health.substr(0, 12), "HTTP/1.1 200");
    EXPECT_NE(health.find("\"ready\":true"), std::string::npos) << health;

    // Anonymous ListBuckets is denied: the S3 auth layer answers, which a
    // health probe alone cannot prove.
    const std::string anonymous = tcit::http_get(rustfs.host(), rustfs.s3_port(), "/");
    EXPECT_EQ(anonymous.substr(0, 12), "HTTP/1.1 403");
}

TEST_F(RustFSModule, CredentialsEnforcedAcrossNetwork) {
    // Declare the network FIRST so RAII tears the containers down before it
    // (a network can't be removed while containers are still attached).
    Network net = Network::create();

    const RustFSContainer rustfs = RustFSImage()
                                       .with_network(net)
                                       .with_network_alias("s3")
                                       .with_access_key("testkey")
                                       .with_secret_key("testsecret1234")
                                       .start();
    // The sibling module is the S3 client here: its image ships mc, and
    // `mc alias set` round-trips an authenticated call — accepted only when
    // the server really consumed the credential env.
    const MinIOContainer mc_host = MinIOImage().with_network(net).start();

    const ExecResult aliased = mc_host.container().exec(
        {"mc", "alias", "set", "rf", "http://s3:9000", "testkey", "testsecret1234"});
    ASSERT_EQ(aliased.exit_code, 0) << aliased.stderr_data;
    const ExecResult made = mc_host.container().exec({"mc", "mb", "rf/credcheck"});
    EXPECT_EQ(made.exit_code, 0) << made.stderr_data;

    const ExecResult rejected = mc_host.container().exec(
        {"mc", "alias", "set", "rfbad", "http://s3:9000", "testkey", "wrongsecret999"});
    EXPECT_NE(rejected.exit_code, 0);
}

TEST_F(RustFSModule, ConsoleServesUnderPrefix) {
    const RustFSContainer rustfs = RustFSImage().start();

    const std::string response =
        tcit::http_get(rustfs.host(), rustfs.console_port(), "/rustfs/console/");
    EXPECT_EQ(response.substr(0, 12), "HTTP/1.1 200");
}
