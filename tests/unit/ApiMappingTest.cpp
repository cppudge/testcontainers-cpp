#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "docker/ApiMapping.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/Healthcheck.hpp"

// Tests in this file:
//   ApiMapping.BuildCreateBodyMinimal - a spec with only an image produces a body with just Image and no optional sections.
//   ApiMapping.BuildCreateBodyFull - cmd, env, labels, exposed ports, and publish-all map to the correct Docker create-body fields.
//   ApiMapping.BuildCreateBodyHealthcheck - a healthcheck maps to a Healthcheck object with the Test array, nanosecond durations, and retries.
//   ApiMapping.BuildCreateBodyNoHealthcheckByDefault - a spec without a healthcheck emits no Healthcheck field.
//   ApiMapping.ParseInspectExtractsStateAndPorts - inspect JSON parses into id, name, running state, and per-port host bindings (null becomes empty).
//   ApiMapping.ParseInspectHealthStatus - inspect JSON with State.Health.Status fills health_status.
//   ApiMapping.ParseInspectNoHealthStatus - inspect JSON without State.Health yields a nullopt health_status.
//   ApiMapping.SplitImage - "name[:tag]" splits into name and tag, defaulting to "latest" and handling a registry host:port.
//   ApiMapping.PullErrorThrows - a pull progress stream containing an error entry throws DockerError.
//   ApiMapping.PullSuccessDoesNotThrow - a clean pull progress stream does not throw.

using namespace testcontainers;
using namespace std::chrono_literals;
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

TEST(ApiMapping, BuildCreateBodyHealthcheck) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.healthcheck = Healthcheck::cmd_shell("exit 0")
                           .with_interval(500ms)
                           .with_timeout(1s)
                           .with_start_period(0ms)
                           .with_retries(3);

    const auto body = build_create_body(spec);
    ASSERT_TRUE(body.contains("Healthcheck"));
    const auto& health = body["Healthcheck"];
    EXPECT_EQ(health["Test"], nlohmann::json({"CMD-SHELL", "exit 0"}));
    // Durations are int64 nanoseconds.
    EXPECT_EQ(health["Interval"].get<std::int64_t>(),
              std::chrono::nanoseconds(500ms).count());
    EXPECT_EQ(health["Timeout"].get<std::int64_t>(), std::chrono::nanoseconds(1s).count());
    EXPECT_EQ(health["StartPeriod"].get<std::int64_t>(), 0);
    EXPECT_EQ(health["Retries"].get<int>(), 3);
}

TEST(ApiMapping, BuildCreateBodyNoHealthcheckByDefault) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    const auto body = build_create_body(spec);
    EXPECT_FALSE(body.contains("Healthcheck"));
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
    EXPECT_FALSE(info.health_status.has_value());  // no Health section here
}

TEST(ApiMapping, ParseInspectHealthStatus) {
    const std::string body = R"({
        "Id": "abc123",
        "State": {"Status": "running", "Running": true,
                  "Health": {"Status": "healthy", "FailingStreak": 0}}
    })";

    const auto info = parse_inspect(body);
    ASSERT_TRUE(info.health_status.has_value());
    EXPECT_EQ(*info.health_status, "healthy");
}

TEST(ApiMapping, ParseInspectNoHealthStatus) {
    const std::string body = R"({
        "Id": "abc123",
        "State": {"Status": "running", "Running": true}
    })";

    const auto info = parse_inspect(body);
    EXPECT_FALSE(info.health_status.has_value());
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
