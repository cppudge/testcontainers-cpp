#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/modules/MinIO.hpp"

// Tests in this file (unit; no Docker daemon — the module's rendering rules
// via to_generic()):
//   MinIOModuleConfig.DefaultRendersPinPortsCmdCredsAndClusterWait - the default config renders the pinned image, exposes 9000+9001/tcp, owns the command `server /data --console-address :9001`, appends the minioadmin credential pair, and installs the /minio/health/cluster probe.
//   MinIOModuleConfig.CredentialPairAppendedLastWinsOverRawEnv - the MINIO_ROOT_* pair lands after pass-through env, so the module's values are the last duplicates (what the image's shell entrypoint applies).
//   MinIOModuleConfig.CredentialRulesThrowAtRender - an access key under 3 characters or a secret key under 8 throws at render (the server's own boot-time rules), before any daemon contact; the minimal valid pair renders.
//   MinIOModuleConfig.BucketsRenderHookAndSortedLabel - with_bucket queues a started hook and a sorted, comma-joined reuse label; an empty bucket name throws at render.
//   MinIOModuleConfig.NoBucketsMeansNoHookAndNoLabel - without with_bucket the render owns no started hook and no label.
//   MinIOModuleConfig.CustomWaitReplacesDefaultProbe - the first with_wait drops the module's health probe; further calls accumulate in order.
//   MinIOModuleConfig.CustomizerRunsLastAndWins - a customizer sees the module-rendered builder and what it sets overrides the module's rendering.
//   MinIOModuleConfig.WithImageRewritesReference - with_image swaps name and tag (bare name defaults to "latest") while the rest of the config survives.
//   MinIOModuleConfig.RenderingIsIdempotent - to_generic() renders into a copy: repeated renders are equal and the config never accumulates env, waits, labels, or hooks.
//   MinIOModuleConfig.PassThroughsLandOnTheImage - the env/label/network/alias/reuse/timeout/attempts pass-throughs reach the rendered builder.

using namespace testcontainers;
using modules::MinIOImage;

TEST(MinIOModuleConfig, DefaultRendersPinPortsCmdCredsAndClusterWait) {
    const GenericImage generic = MinIOImage().to_generic();

    EXPECT_EQ(generic.image(), "minio/minio");
    EXPECT_EQ(generic.tag(), "RELEASE.2025-09-07T16-13-09Z");
    ASSERT_EQ(generic.exposed_ports().size(), 2u);
    EXPECT_EQ(generic.exposed_ports()[0], tcp(9000));
    EXPECT_EQ(generic.exposed_ports()[1], tcp(9001));

    // The module owns the command: the fixed console address is what makes
    // exposing 9001 possible at all.
    EXPECT_EQ(generic.cmd(),
              (std::vector<std::string>{"server", "/data", "--console-address", ":9001"}));

    ASSERT_EQ(generic.env().size(), 2u);
    EXPECT_EQ(generic.env()[0],
              (std::pair<std::string, std::string>{"MINIO_ROOT_USER", "minioadmin"}));
    EXPECT_EQ(generic.env()[1],
              (std::pair<std::string, std::string>{"MINIO_ROOT_PASSWORD", "minioadmin"}));

    // The one health endpoint gated on the object layer being writable.
    ASSERT_EQ(generic.waits().size(), 1u);
    const auto* http = std::get_if<wait_for::Http>(&generic.waits()[0]);
    ASSERT_NE(http, nullptr);
    EXPECT_EQ(http->path, "/minio/health/cluster");
    EXPECT_EQ(http->port, tcp(9000));
    EXPECT_EQ(http->expected_status, 200);

    EXPECT_TRUE(generic.started_hooks().empty());
    EXPECT_TRUE(generic.labels().empty());
}

TEST(MinIOModuleConfig, CredentialPairAppendedLastWinsOverRawEnv) {
    MinIOImage cfg;
    cfg.with_env("MINIO_ROOT_USER", "shadowed").with_env("TZ", "UTC").with_access_key("tc-admin");

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.env().size(), 4u);
    // Pass-through entries first, the managed pair last: the image's shell
    // entrypoint applies the LAST duplicate of a key.
    EXPECT_EQ(generic.env()[0].second, "shadowed");
    EXPECT_EQ(generic.env()[2],
              (std::pair<std::string, std::string>{"MINIO_ROOT_USER", "tc-admin"}));
    EXPECT_EQ(generic.env()[3].first, "MINIO_ROOT_PASSWORD");
}

TEST(MinIOModuleConfig, CredentialRulesThrowAtRender) {
    // The server's own boot-time rules, surfaced before any daemon contact.
    EXPECT_THROW(MinIOImage().with_access_key("ab").to_generic(), Error);
    EXPECT_THROW(MinIOImage().with_access_key("").to_generic(), Error);
    EXPECT_THROW(MinIOImage().with_secret_key("1234567").to_generic(), Error);
    EXPECT_THROW(MinIOImage().with_secret_key("").to_generic(), Error);
    // The minimal valid pair renders.
    EXPECT_NO_THROW(MinIOImage().with_access_key("abc").with_secret_key("12345678").to_generic());
}

TEST(MinIOModuleConfig, BucketsRenderHookAndSortedLabel) {
    MinIOImage cfg;
    cfg.with_bucket("zulu").with_bucket("alpha");
    ASSERT_EQ(cfg.buckets(), (std::vector<std::string>{"zulu", "alpha"}));

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.started_hooks().size(), 1u);
    // Sorted: the reuse hash must not depend on registration order.
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0], (std::pair<std::string, std::string>{
                                       "org.testcontainers.minio.buckets", "alpha,zulu"}));

    EXPECT_THROW(MinIOImage().with_bucket("").to_generic(), Error);
}

TEST(MinIOModuleConfig, NoBucketsMeansNoHookAndNoLabel) {
    const GenericImage generic = MinIOImage().to_generic();
    EXPECT_TRUE(generic.started_hooks().empty());
    EXPECT_TRUE(generic.labels().empty());
}

TEST(MinIOModuleConfig, CustomWaitReplacesDefaultProbe) {
    MinIOImage cfg;
    cfg.with_wait(wait_for::log("1 Online"));
    cfg.with_wait(wait_for::listening_port(tcp(9000)));

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.waits().size(), 2u); // the default health probe is gone
    EXPECT_NE(std::get_if<wait_for::LogMessage>(&generic.waits()[0]), nullptr);
    EXPECT_NE(std::get_if<wait_for::Port>(&generic.waits()[1]), nullptr);
}

TEST(MinIOModuleConfig, CustomizerRunsLastAndWins) {
    MinIOImage cfg;
    cfg.with_customizer([](GenericImage& generic) {
           // Runs after the module's rendering, so the credential pair and
           // the command are already applied.
           ASSERT_EQ(generic.env().size(), 2u);
           ASSERT_EQ(generic.cmd().size(), 4u);
           generic.with_label("team", "storage");
       })
        .with_customizer([](GenericImage& generic) { generic.with_env("MINIO_REGION", "eu-x"); });

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.env().size(), 3u); // customizer env lands after the pair
    EXPECT_EQ(generic.env()[2].first, "MINIO_REGION");
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
}

TEST(MinIOModuleConfig, WithImageRewritesReference) {
    MinIOImage cfg;
    cfg.with_access_key("tc-admin").with_image("mirror.example/minio:patched");

    const GenericImage generic = cfg.to_generic();
    EXPECT_EQ(generic.image(), "mirror.example/minio");
    EXPECT_EQ(generic.tag(), "patched");
    // The rest of the config survives the swap.
    ASSERT_EQ(generic.env().size(), 2u);
    EXPECT_EQ(generic.env()[0].second, "tc-admin");

    cfg.with_image("minio/minio"); // bare name defaults to "latest"
    EXPECT_EQ(cfg.to_generic().image(), "minio/minio");
    EXPECT_EQ(cfg.to_generic().tag(), "latest");
}

TEST(MinIOModuleConfig, RenderingIsIdempotent) {
    MinIOImage cfg;
    cfg.with_bucket("seeded");

    const GenericImage first = cfg.to_generic();
    const GenericImage second = cfg.to_generic();
    EXPECT_EQ(first.env(), second.env());
    ASSERT_EQ(second.waits().size(), 1u);         // the probe is rendered once
    ASSERT_EQ(second.labels().size(), 1u);        // the label is applied once
    ASSERT_EQ(second.started_hooks().size(), 1u); // the hook is queued once
}

TEST(MinIOModuleConfig, PassThroughsLandOnTheImage) {
    MinIOImage cfg;
    cfg.with_env("MINIO_BROWSER", "off")
        .with_label("team", "storage")
        .with_network("net-a")
        .with_network_alias("s3")
        .with_reuse()
        .with_startup_timeout(std::chrono::seconds(90))
        .with_startup_attempts(2);

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.env().size(), 3u); // MINIO_BROWSER + the managed pair
    EXPECT_EQ(generic.env()[0].first, "MINIO_BROWSER");
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
    EXPECT_EQ(generic.network().value_or(""), "net-a");
    ASSERT_EQ(generic.network_aliases().size(), 1u);
    EXPECT_EQ(generic.network_aliases()[0], "s3");
    EXPECT_TRUE(generic.reuse());
    EXPECT_EQ(generic.startup_timeout(), std::chrono::seconds(90));
    EXPECT_EQ(generic.startup_attempts(), 2);
}
