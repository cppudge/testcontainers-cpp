#include <gtest/gtest.h>

#include <string>

#include "testcontainers/ExecResult.hpp"
#include "testcontainers/modules/OpenSearch.hpp"

#include "EngineGuard.hpp"
#include "HttpGet.hpp"

// Tests in this file (integration; require a Linux-containers Docker daemon):
//   OpenSearchModule.DefaultsStartAndServe - a default OpenSearchImage starts, http_url() renders the endpoint shape, and the root endpoint answers with the project tagline through the in-image curl.
//   OpenSearchModule.ClusterHealthAnswersFromHost - a raw host-side GET /_cluster/health through the published port answers 200 with a real health report (pins the default probe's semantics; the status VALUE right after readiness races between yellow and green — an internal shard may still be initializing — so only the shape is asserted).
//   OpenSearchModule.IndexSearchRoundTrip - a document PUT with ?refresh=true is immediately searchable; the search reports exactly one hit.
//   OpenSearchModule.DottedEnvBecomesSetting - with_env("cluster.name", ...) reaches the server as a real setting (the entrypoint turns dotted keys into -E options).
//   OpenSearchModule.HeapOverrideWins - a user OPENSEARCH_JAVA_OPTS duplicate wins over the module's (the server boots with the smaller heap and still serves).

using namespace testcontainers;
using modules::OpenSearchContainer;
using modules::OpenSearchImage;

// Requires a Linux-containers daemon; skipped otherwise.
class OpenSearchModule : public tcit::LinuxEngineTest {};

TEST_F(OpenSearchModule, DefaultsStartAndServe) {
    const OpenSearchContainer search = OpenSearchImage().start();

    EXPECT_EQ(search.http_url(), "http://" + search.host() + ":" + std::to_string(search.port()));

    const ExecResult root = search.container().exec({"curl", "-s", "http://localhost:9200/"});
    ASSERT_EQ(root.exit_code, 0) << root.stderr_data;
    EXPECT_NE(root.stdout_data.find("The OpenSearch Project: https://opensearch.org/"),
              std::string::npos)
        << root.stdout_data;
}

TEST_F(OpenSearchModule, ClusterHealthAnswersFromHost) {
    const OpenSearchContainer search = OpenSearchImage().start();

    // 200 with a real health report is the probe's promise. The status VALUE
    // is deliberately not pinned: right after readiness it races between
    // yellow and green (an internal shard may still be initializing —
    // observed live).
    const std::string response = tcit::http_get(search.host(), search.port(), "/_cluster/health");
    EXPECT_EQ(response.substr(0, 12), "HTTP/1.1 200");
    EXPECT_NE(response.find("\"cluster_name\""), std::string::npos) << response;
    EXPECT_NE(response.find("\"status\":\""), std::string::npos) << response;
}

TEST_F(OpenSearchModule, IndexSearchRoundTrip) {
    const OpenSearchContainer search = OpenSearchImage().start();

    // ?refresh=true makes the document searchable immediately (no 1s
    // refresh-interval race).
    const ExecResult put = search.container().exec(
        {"curl", "-s", "-XPUT", "http://localhost:9200/idx/_doc/1?refresh=true", "-H",
         "Content-Type: application/json", "-d", R"({"msg":"hello"})"});
    ASSERT_EQ(put.exit_code, 0) << put.stderr_data;
    EXPECT_NE(put.stdout_data.find("\"result\":\"created\""), std::string::npos) << put.stdout_data;

    const ExecResult found =
        search.container().exec({"curl", "-s", "http://localhost:9200/idx/_search?q=msg:hello"});
    ASSERT_EQ(found.exit_code, 0) << found.stderr_data;
    EXPECT_NE(found.stdout_data.find("\"total\":{\"value\":1"), std::string::npos)
        << found.stdout_data;
}

TEST_F(OpenSearchModule, DottedEnvBecomesSetting) {
    const OpenSearchContainer search =
        OpenSearchImage().with_env("cluster.name", "tc-opensearch").start();

    const ExecResult root = search.container().exec({"curl", "-s", "http://localhost:9200/"});
    ASSERT_EQ(root.exit_code, 0) << root.stderr_data;
    EXPECT_NE(root.stdout_data.find("\"cluster_name\" : \"tc-opensearch\""), std::string::npos)
        << root.stdout_data;
}

TEST_F(OpenSearchModule, HeapOverrideWins) {
    // The user duplicate lands after the module's key, and the entrypoint
    // applies the last one — observed end to end: the server boots with the
    // smaller heap and serves.
    const OpenSearchContainer search =
        OpenSearchImage().with_env("OPENSEARCH_JAVA_OPTS", "-Xms256m -Xmx256m").start();

    const ExecResult root = search.container().exec({"curl", "-s", "http://localhost:9200/"});
    ASSERT_EQ(root.exit_code, 0) << root.stderr_data;
    EXPECT_NE(root.stdout_data.find("The OpenSearch Project"), std::string::npos)
        << root.stdout_data;
}
