#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/modules/OpenSearch.hpp"

// Tests in this file (unit; no Docker daemon — the module's rendering rules
// via to_generic()):
//   OpenSearchModuleConfig.DefaultRendersPinPortEnvAndHealthWait - the default config renders the pinned image, exposes 9200/tcp only, bakes the four managed env keys (single-node discovery, both security-off switches, the 512m heap), installs the /_cluster/health probe, raises the budget to 120s, and owns no command.
//   OpenSearchModuleConfig.UserEnvAppendsAfterManagedSoUserWins - with_env lands after the constructor-baked managed keys, so on a duplicate the user's (later) entry is what the entrypoint applies.
//   OpenSearchModuleConfig.CustomWaitReplacesDefaultProbe - the first with_wait drops the module's health probe; further calls accumulate in order.
//   OpenSearchModuleConfig.CustomizerRunsLastAndWins - a customizer sees the module-rendered builder and what it sets overrides the module's rendering.
//   OpenSearchModuleConfig.WithImageRewritesReference - with_image swaps name and tag (bare name defaults to "latest") while the managed env survives.
//   OpenSearchModuleConfig.RenderingIsIdempotent - to_generic() renders into a copy: repeated renders are equal and the constructor-baked env is never re-appended.
//   OpenSearchModuleConfig.PassThroughsLandOnTheImage - the label/network/alias/reuse/timeout/attempts pass-throughs reach the rendered builder.

using namespace testcontainers;
using modules::OpenSearchImage;

TEST(OpenSearchModuleConfig, DefaultRendersPinPortEnvAndHealthWait) {
    const GenericImage generic = OpenSearchImage().to_generic();

    EXPECT_EQ(generic.image(), "opensearchproject/opensearch");
    EXPECT_EQ(generic.tag(), "3.7.0");
    ASSERT_EQ(generic.exposed_ports().size(), 1u);
    EXPECT_EQ(generic.exposed_ports()[0], tcp(9200));

    // The managed quartet, in constructor order; dotted keys become -E
    // settings in the entrypoint.
    ASSERT_EQ(generic.env().size(), 4u);
    EXPECT_EQ(generic.env()[0],
              (std::pair<std::string, std::string>{"discovery.type", "single-node"}));
    EXPECT_EQ(generic.env()[1],
              (std::pair<std::string, std::string>{"DISABLE_SECURITY_PLUGIN", "true"}));
    EXPECT_EQ(generic.env()[2],
              (std::pair<std::string, std::string>{"DISABLE_INSTALL_DEMO_CONFIG", "true"}));
    EXPECT_EQ(generic.env()[3],
              (std::pair<std::string, std::string>{"OPENSEARCH_JAVA_OPTS", "-Xms512m -Xmx512m"}));

    // 200 from cluster health means a cluster manager is elected — what the
    // first index/search call needs; the root endpoint answers earlier.
    ASSERT_EQ(generic.waits().size(), 1u);
    const auto* http = std::get_if<wait_for::Http>(&generic.waits()[0]);
    ASSERT_NE(http, nullptr);
    EXPECT_EQ(http->path, "/_cluster/health");
    EXPECT_EQ(http->port, tcp(9200));
    EXPECT_EQ(http->expected_status, 200);

    // The image's own entrypoint command keeps applying; a JVM boot needs
    // the raised budget.
    EXPECT_TRUE(generic.cmd().empty());
    EXPECT_EQ(generic.startup_timeout(), std::chrono::seconds(120));
    EXPECT_TRUE(generic.started_hooks().empty());
}

TEST(OpenSearchModuleConfig, UserEnvAppendsAfterManagedSoUserWins) {
    OpenSearchImage cfg;
    cfg.with_env("OPENSEARCH_JAVA_OPTS", "-Xms256m -Xmx256m").with_env("cluster.name", "tc-it");

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.env().size(), 6u);
    // Managed keys first (constructor-baked), user entries after: the
    // entrypoint applies the LAST duplicate, so the user's heap wins.
    EXPECT_EQ(generic.env()[3].second, "-Xms512m -Xmx512m");
    EXPECT_EQ(generic.env()[4],
              (std::pair<std::string, std::string>{"OPENSEARCH_JAVA_OPTS", "-Xms256m -Xmx256m"}));
    EXPECT_EQ(generic.env()[5].first, "cluster.name");
}

TEST(OpenSearchModuleConfig, CustomWaitReplacesDefaultProbe) {
    OpenSearchImage cfg;
    cfg.with_wait(wait_for::log("started"));
    cfg.with_wait(wait_for::listening_port(tcp(9200)));

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.waits().size(), 2u); // the default health probe is gone
    EXPECT_NE(std::get_if<wait_for::LogMessage>(&generic.waits()[0]), nullptr);
    EXPECT_NE(std::get_if<wait_for::Port>(&generic.waits()[1]), nullptr);
}

TEST(OpenSearchModuleConfig, CustomizerRunsLastAndWins) {
    OpenSearchImage cfg;
    cfg.with_customizer([](GenericImage& generic) {
           // Runs after the module's rendering, so the managed env and the
           // probe are already applied.
           ASSERT_EQ(generic.env().size(), 4u);
           ASSERT_EQ(generic.waits().size(), 1u);
           generic.with_label("team", "search");
       })
        .with_customizer([](GenericImage& generic) { generic.with_env("TZ", "UTC"); });

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.env().size(), 5u); // customizer env lands after the quartet
    EXPECT_EQ(generic.env()[4].first, "TZ");
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
}

TEST(OpenSearchModuleConfig, WithImageRewritesReference) {
    OpenSearchImage cfg;
    cfg.with_image("opensearchproject/opensearch:2.19.6");

    const GenericImage generic = cfg.to_generic();
    EXPECT_EQ(generic.image(), "opensearchproject/opensearch");
    EXPECT_EQ(generic.tag(), "2.19.6");
    // The managed env survives the swap.
    ASSERT_EQ(generic.env().size(), 4u);
    EXPECT_EQ(generic.env()[0].second, "single-node");

    cfg.with_image("opensearchproject/opensearch"); // bare name defaults to "latest"
    EXPECT_EQ(cfg.to_generic().image(), "opensearchproject/opensearch");
    EXPECT_EQ(cfg.to_generic().tag(), "latest");
}

TEST(OpenSearchModuleConfig, RenderingIsIdempotent) {
    OpenSearchImage cfg;
    cfg.with_env("cluster.name", "tc-it");

    const GenericImage first = cfg.to_generic();
    const GenericImage second = cfg.to_generic();
    EXPECT_EQ(first.env(), second.env());
    ASSERT_EQ(second.env().size(), 5u);   // the quartet was baked once, in the ctor
    ASSERT_EQ(second.waits().size(), 1u); // the probe is rendered once
}

TEST(OpenSearchModuleConfig, PassThroughsLandOnTheImage) {
    OpenSearchImage cfg;
    cfg.with_label("team", "search")
        .with_network("net-a")
        .with_network_alias("search")
        .with_reuse()
        .with_startup_timeout(std::chrono::seconds(90))
        .with_startup_attempts(2);

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
    EXPECT_EQ(generic.network().value_or(""), "net-a");
    ASSERT_EQ(generic.network_aliases().size(), 1u);
    EXPECT_EQ(generic.network_aliases()[0], "search");
    EXPECT_TRUE(generic.reuse());
    EXPECT_EQ(generic.startup_timeout(), std::chrono::seconds(90)); // user override wins
    EXPECT_EQ(generic.startup_attempts(), 2);
}
