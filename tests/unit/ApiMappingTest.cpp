#include <gtest/gtest.h>

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "docker/ApiMapping.hpp"
#include "testcontainers/Error.hpp"

using namespace testcontainers;
using testcontainers::docker::build_create_body;
using testcontainers::docker::parse_inspect;
using testcontainers::docker::split_image;
using testcontainers::docker::throw_if_pull_error;

TEST(ApiMapping, BuildCreateBodyMinimal) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";

    const auto body = build_create_body(spec);
    EXPECT_EQ(body["Image"], "alpine:3.20");
    EXPECT_FALSE(body.contains("Cmd"));
    EXPECT_FALSE(body.contains("Env"));
    EXPECT_FALSE(body.contains("HostConfig"));
}

TEST(ApiMapping, BuildCreateBodyFull) {
    CreateContainerSpec spec;
    spec.image = "redis:7.2";
    spec.cmd = {"redis-server", "--port", "6379"};
    spec.env = {"FOO=bar"};
    spec.labels = {{"k", "v"}};
    spec.exposed_ports = {"6379/tcp"};
    spec.publish_all_ports = true;

    const auto body = build_create_body(spec);
    EXPECT_EQ(body["Cmd"], nlohmann::json({"redis-server", "--port", "6379"}));
    EXPECT_EQ(body["Env"][0], "FOO=bar");
    EXPECT_EQ(body["Labels"]["k"], "v");
    EXPECT_TRUE(body["ExposedPorts"].contains("6379/tcp"));
    EXPECT_TRUE(body["HostConfig"]["PublishAllPorts"].get<bool>());
}

TEST(ApiMapping, ParseInspectExtractsStateAndPorts) {
    const std::string body = R"({
        "Id": "abc123",
        "Name": "/funny_name",
        "State": {"Status": "running", "Running": true, "ExitCode": 0},
        "NetworkSettings": {"Ports": {
            "6379/tcp": [{"HostIp": "0.0.0.0", "HostPort": "32768"},
                         {"HostIp": "::", "HostPort": "32769"}],
            "80/tcp": null
        }}
    })";

    const auto info = parse_inspect(body);
    EXPECT_EQ(info.id, "abc123");
    EXPECT_EQ(info.name, "/funny_name");
    EXPECT_EQ(info.status, "running");
    EXPECT_TRUE(info.running);

    ASSERT_EQ(info.ports.count("6379/tcp"), 1u);
    ASSERT_EQ(info.ports.at("6379/tcp").size(), 2u);
    EXPECT_EQ(info.ports.at("6379/tcp")[0].host_ip, "0.0.0.0");
    EXPECT_EQ(info.ports.at("6379/tcp")[0].host_port, 32768);
    EXPECT_EQ(info.ports.at("6379/tcp")[1].host_port, 32769);
    EXPECT_TRUE(info.ports.at("80/tcp").empty()); // null bindings -> empty
}

TEST(ApiMapping, SplitImage) {
    using P = std::pair<std::string, std::string>;
    EXPECT_EQ(split_image("alpine"), (P{"alpine", "latest"}));
    EXPECT_EQ(split_image("alpine:3.20"), (P{"alpine", "3.20"}));
    EXPECT_EQ(split_image("ghcr.io/owner/img:1.2"), (P{"ghcr.io/owner/img", "1.2"}));
    EXPECT_EQ(split_image("my-reg:5000/img"), (P{"my-reg:5000/img", "latest"}));
}

TEST(ApiMapping, PullErrorThrows) {
    const std::string stream =
        R"({"status":"Pulling from library/x"})"
        "\n"
        R"({"errorDetail":{"message":"manifest unknown"},"error":"manifest unknown"})"
        "\n";
    EXPECT_THROW(throw_if_pull_error(stream, "x:y"), DockerError);
}

TEST(ApiMapping, PullSuccessDoesNotThrow) {
    const std::string stream =
        R"({"status":"Pulling from library/alpine"})"
        "\n"
        R"({"status":"Download complete"})"
        "\n";
    EXPECT_NO_THROW(throw_if_pull_error(stream, "alpine:3.20"));
}
