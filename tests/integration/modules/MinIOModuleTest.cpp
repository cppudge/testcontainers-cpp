#include <gtest/gtest.h>

#include <string>

#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/modules/MinIO.hpp"

#include "EngineGuard.hpp"
#include "HttpGet.hpp"

// Tests in this file (integration; require a Linux-containers Docker daemon):
//   MinIOModule.DefaultsStartAndObjectRoundTrip - a default MinIOImage with one with_bucket starts, the getters report the minioadmin pair and the URL shapes, and a real S3 PUT/GET round-trips through the in-image mc (which the bucket hook already aliased to the server as `tc`).
//   MinIOModule.HealthAnswersFromHost - a raw host-side GET /minio/health/live through the published S3 port answers 200 (the end-to-end proof a bare TCP connect cannot give).
//   MinIOModule.BucketsFromRegistrationVisibleViaLs - both with_bucket buckets exist on the server after start() (listed by the in-image mc).
//   MinIOModule.CustomCredentialsEnforced - a credential pair full of URL-hostile characters reaches the server (the bucket hook authenticates with it as plain argv), and a wrong-credentials alias probe is rejected.
//   MinIOModule.ConsoleAnswersFromHost - the published console port serves the web UI (GET / answers 200).

using namespace testcontainers;
using modules::MinIOContainer;
using modules::MinIOImage;

// Requires a Linux-containers daemon; skipped otherwise.
class MinIOModule : public tcit::LinuxEngineTest {};

TEST_F(MinIOModule, DefaultsStartAndObjectRoundTrip) {
    const MinIOContainer minio = MinIOImage().with_bucket("roundtrip").start();

    EXPECT_EQ(minio.access_key(), "minioadmin");
    EXPECT_EQ(minio.secret_key(), "minioadmin");
    EXPECT_EQ(minio.s3_url(), "http://" + minio.host() + ":" + std::to_string(minio.s3_port()));
    EXPECT_EQ(minio.console_url(),
              "http://" + minio.host() + ":" + std::to_string(minio.console_port()));

    // A real S3 PUT/GET, no C++ driver: the bucket hook already pointed the
    // in-image mc at the server (alias `tc`).
    minio.container().copy_to(CopyToContainer::content("hello from tc\n", "/tmp/hello.txt"));
    const ExecResult put = minio.container().exec({"mc", "cp", "/tmp/hello.txt", "tc/roundtrip/"});
    ASSERT_EQ(put.exit_code, 0) << put.stderr_data;
    const ExecResult get = minio.container().exec({"mc", "cat", "tc/roundtrip/hello.txt"});
    EXPECT_EQ(get.exit_code, 0) << get.stderr_data;
    EXPECT_EQ(get.stdout_data, "hello from tc\n");
}

TEST_F(MinIOModule, HealthAnswersFromHost) {
    const MinIOContainer minio = MinIOImage().start();

    const std::string response =
        tcit::http_get(minio.host(), minio.s3_port(), "/minio/health/live");
    EXPECT_EQ(response.substr(0, 12), "HTTP/1.1 200");
}

TEST_F(MinIOModule, BucketsFromRegistrationVisibleViaLs) {
    const MinIOContainer minio =
        MinIOImage().with_bucket("seeded-zulu").with_bucket("seeded-alpha").start();

    const ExecResult ls = minio.container().exec({"mc", "ls", "tc"});
    ASSERT_EQ(ls.exit_code, 0) << ls.stderr_data;
    EXPECT_NE(ls.stdout_data.find("seeded-zulu/"), std::string::npos) << ls.stdout_data;
    EXPECT_NE(ls.stdout_data.find("seeded-alpha/"), std::string::npos) << ls.stdout_data;
}

TEST_F(MinIOModule, CustomCredentialsEnforced) {
    // URL-hostile characters on purpose: the hook hands credentials to mc as
    // plain argv, so nothing needs URL encoding anywhere. The hook itself is
    // the positive proof — aliasing round-trips an authenticated call, and a
    // failed hook would have failed start().
    const MinIOContainer minio = MinIOImage()
                                     .with_access_key("tc-admin")
                                     .with_secret_key("s3cr@t/pw+8")
                                     .with_bucket("authed")
                                     .start();

    const ExecResult ls = minio.container().exec({"mc", "ls", "tc"});
    ASSERT_EQ(ls.exit_code, 0) << ls.stderr_data;
    EXPECT_NE(ls.stdout_data.find("authed/"), std::string::npos) << ls.stdout_data;

    // The negative: a wrong secret is rejected by the same authenticated
    // round trip the hook relies on.
    const ExecResult bad = minio.container().exec(
        {"mc", "alias", "set", "bad", "http://127.0.0.1:9000", "tc-admin", "wrongwrong12"});
    EXPECT_NE(bad.exit_code, 0);
}

TEST_F(MinIOModule, ConsoleAnswersFromHost) {
    const MinIOContainer minio = MinIOImage().start();

    const std::string response = tcit::http_get(minio.host(), minio.console_port(), "/");
    EXPECT_EQ(response.substr(0, 12), "HTTP/1.1 200");
}
