#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "docker/ApiMapping.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/ExecOptions.hpp"
#include "testcontainers/Healthcheck.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/Ulimit.hpp"

// Tests in this file:
//   ApiMapping.BuildCreateBodyMinimal - a spec with only an image produces a body with just Image and no optional sections.
//   ApiMapping.BuildCreateBodyFull - cmd, env, labels, exposed ports, and publish-all map to the correct Docker create-body fields.
//   ApiMapping.BuildCreateBodyHealthcheck - a healthcheck maps to a Healthcheck object with the Test array, nanosecond durations, and retries.
//   ApiMapping.BuildCreateBodyNoHealthcheckByDefault - a spec without a healthcheck emits no Healthcheck field.
//   ApiMapping.BuildCreateBodyProcessConfig - entrypoint, working dir, and user map to Entrypoint, WorkingDir, and User.
//   ApiMapping.BuildCreateBodyProcessConfigOmittedByDefault - entrypoint/working dir/user are absent when unset.
//   ApiMapping.BuildCreateBodyTty - the tty flag maps to a top-level "Tty":true (sibling of Cmd/User, not under HostConfig) and is absent by default.
//   ApiMapping.BuildCreateBodyPrivileged - the privileged flag maps to HostConfig.Privileged.
//   ApiMapping.BuildCreateBodyIsolation - the isolation field maps to HostConfig.Isolation and is absent by default.
//   ApiMapping.BuildCreateBodyAutoRemove - the auto_remove flag maps to HostConfig.AutoRemove and is absent by default.
//   ApiMapping.BuildCreateBodyBindMount - a read-only bind mount maps to a HostConfig.Mounts entry with Type/Source/Target/ReadOnly and no TmpfsOptions.
//   ApiMapping.BuildCreateBodyVolumeMount - a volume mount maps to a HostConfig.Mounts entry with Type=volume and Source set to the volume name.
//   ApiMapping.BuildCreateBodyTmpfsMount - a tmpfs mount maps to Type=tmpfs with no Source and TmpfsOptions SizeBytes/Mode.
//   ApiMapping.BuildCreateBodyNoMountsByDefault - a spec without mounts/privileged emits no HostConfig.Mounts or Privileged.
//   ApiMapping.BuildCreateBodyNetworkMode - a set network maps to HostConfig.NetworkMode.
//   ApiMapping.BuildCreateBodyNoNetworkModeByDefault - a spec without a network emits no HostConfig.NetworkMode.
//   ApiMapping.BuildCreateBodyNetworkAliases - a network plus aliases maps to NetworkingConfig.EndpointsConfig.<net>.Aliases, and aliases without a network emit no NetworkingConfig.
//   ApiMapping.BuildCreateBodyHostConfigKnobs - memory, shm size, ulimits, cap add/drop, and extra hosts map to their HostConfig fields.
//   ApiMapping.BuildNetworkCreateBody - a full NetworkCreateSpec maps to Name, Driver, Internal, Attachable, EnableIPv6, IPAM.Config[0].Subnet/Gateway, Options, and Labels.
//   ApiMapping.BuildNetworkCreateBodyMinimal - a NetworkCreateSpec with only a name emits just Name and no Driver/IPAM/flags.
//   ApiMapping.BuildConnectNetworkBody - the connect body carries Container, adding EndpointConfig.Aliases only when aliases are given.
//   ApiMapping.BuildVolumeCreateBody - a full VolumeCreateSpec maps to Name, Driver, DriverOpts, and Labels.
//   ApiMapping.BuildVolumeCreateBodyMinimal - a VolumeCreateSpec with only a name emits just Name and no Driver/DriverOpts/Labels.
//   ApiMapping.ParseVolumeInspect - GET /volumes/{name} JSON parses Name/Driver/Mountpoint/Scope and the Labels/Options maps.
//   ApiMapping.ParseVolumeInspectNullMaps - null Labels/Options parse into empty maps.
//   ApiMapping.BuildCreateBodyPatchDeepMerges - create_body_patch deep-merges into the body, keeping existing fields while adding nested and top-level ones.
//   ApiMapping.BuildCreateBodyPatchInvalidThrows - an invalid-JSON create_body_patch makes build_create_body throw DockerError.
//   ApiMapping.BuildCreateQueryEmptyByDefault - a spec with neither name nor platform yields an empty create query.
//   ApiMapping.BuildCreateQueryName - a spec with only a name yields "?name=<encoded>".
//   ApiMapping.BuildCreateQueryPlatform - a spec with a platform yields "?platform=<encoded>" (slash percent-encoded).
//   ApiMapping.BuildCreateQueryNameAndPlatform - a spec with both name and platform joins them with '&'.
//   ApiMapping.ParseServerOs - GET /version JSON parses the Os field ("windows"/"linux"), defaulting to "" when absent.
//   ApiMapping.BuildExecCreateBody - the exec-create body carries Cmd and AttachStdout/AttachStderr set true.
//   ApiMapping.BuildExecCreateBodyDefaultsOmitOptions - with default ExecOptions the body sets Tty=false and omits AttachStdin/Env/WorkingDir/User/Privileged.
//   ApiMapping.BuildExecCreateBodyWithOptions - env/working_dir/user/privileged/tty map to Env/WorkingDir/User/Privileged/Tty.
//   ApiMapping.BuildExecCreateBodyStdinAttaches - a set stdin_data adds AttachStdin=true (and is absent otherwise).
//   ApiMapping.ParseExecExitCode - exec-inspect JSON parses ExitCode into the integer result (defaulting to 0 when absent).
//   ApiMapping.ParseInspectExtractsStateAndPorts - inspect JSON parses into id, name, running state, and per-port host bindings (null becomes empty).
//   ApiMapping.ParseInspectTty - inspect JSON with Config.Tty=true parses into ContainerInspect.tty (false when absent).
//   ApiMapping.ParseInspectHealthStatus - inspect JSON with State.Health.Status fills health_status.
//   ApiMapping.ParseInspectNoHealthStatus - inspect JSON without State.Health yields a nullopt health_status.
//   ApiMapping.ParseInspectMalformedHostPort - a non-numeric, out-of-range, or trailing-garbage HostPort drops that binding (valid siblings survive) instead of throwing or surviving as port 0.
//   ApiMapping.ParseContainerList - a /containers/json array parses into ContainerSummary entries with id, names, image, state, and labels.
//   ApiMapping.SplitImage - "name[:tag]" splits into name and tag, defaulting to "latest" and handling a registry host:port.
//   ApiMapping.PullErrorThrows - a pull progress stream containing an error entry throws DockerError.
//   ApiMapping.PullSuccessDoesNotThrow - a clean pull progress stream does not throw.
//   ApiMapping.PullNonStringErrorThrows - a pull stream entry whose "error" is not a string still throws DockerError (dumped payload), never a raw json type_error.
//   ApiMapping.BuildErrorThrows - a build stream containing error/errorDetail throws DockerError.
//   ApiMapping.BuildSuccessDoesNotThrow - a clean build progress stream does not throw.
//   ApiMapping.BuildQueryBasics - build_build_query always emits t, dockerfile and forcerm=1 (a failed build's intermediate container must not leak) and includes nocache/pull/target only when set.
//   ApiMapping.BuildQueryBuildArgs - a build_arg yields a buildargs= value that URL-decodes to the JSON map.
//   ApiMapping.ExpectStringFieldExtracts - expect_string_field returns the named top-level string field.
//   ApiMapping.ExpectStringFieldWrapsFailures - a malformed body, a missing field, and a non-string field all throw DockerError carrying the context, never raw nlohmann exceptions.

using namespace testcontainers;
using namespace std::chrono_literals;
using testcontainers::docker::build_build_query;
using testcontainers::docker::build_create_body;
using testcontainers::docker::build_create_query;
using testcontainers::docker::build_exec_create_body;
using testcontainers::docker::build_connect_network_body;
using testcontainers::docker::build_network_create_body;
using testcontainers::docker::build_volume_create_body;
using testcontainers::docker::parse_volume_inspect;
using testcontainers::docker::BuildOptions;
using testcontainers::docker::parse_container_list;
using testcontainers::docker::parse_exec_exit_code;
using testcontainers::docker::parse_inspect;
using testcontainers::docker::parse_server_os;
using testcontainers::docker::split_image;
using testcontainers::docker::expect_string_field;
using testcontainers::docker::throw_if_build_error;
using testcontainers::docker::throw_if_pull_error;

namespace {

// A trivial percent-encoder for the create-query tests: encodes '/' as %2F so
// the platform value is visibly distinguishable from the raw string. Mirrors
// DockerClient's url_encode for the characters these tests exercise.
std::string test_encode(const std::string& value) {
    std::string out;
    for (const char c : value) {
        if (c == '/') {
            out += "%2F";
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

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

TEST(ApiMapping, BuildCreateBodyProcessConfig) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.entrypoint = {"echo"};
    spec.working_dir = "/tmp";
    spec.user = "1000:1000";

    const auto body = build_create_body(spec);
    EXPECT_EQ(body["Entrypoint"], nlohmann::json({"echo"}));
    EXPECT_EQ(body["WorkingDir"], "/tmp");
    EXPECT_EQ(body["User"], "1000:1000");
}

TEST(ApiMapping, BuildCreateBodyProcessConfigOmittedByDefault) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    const auto body = build_create_body(spec);
    EXPECT_FALSE(body.contains("Entrypoint"));
    EXPECT_FALSE(body.contains("WorkingDir"));
    EXPECT_FALSE(body.contains("User"));
}

TEST(ApiMapping, BuildCreateBodyTty) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.tty = true;

    const auto body = build_create_body(spec);
    // Tty is a top-level container Config field (sibling of Cmd/User), NOT under
    // HostConfig.
    ASSERT_TRUE(body.contains("Tty"));
    EXPECT_TRUE(body["Tty"].get<bool>());
    EXPECT_FALSE(body.contains("HostConfig")); // tty alone adds no HostConfig

    // Absent by default (matches how Privileged / other bools behave).
    CreateContainerSpec plain;
    plain.image = "alpine:3.20";
    EXPECT_FALSE(build_create_body(plain).contains("Tty"));
}

TEST(ApiMapping, BuildCreateBodyPrivileged) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.privileged = true;

    const auto body = build_create_body(spec);
    ASSERT_TRUE(body.contains("HostConfig"));
    EXPECT_TRUE(body["HostConfig"]["Privileged"].get<bool>());
}

TEST(ApiMapping, BuildCreateBodyIsolation) {
    CreateContainerSpec spec;
    spec.image = "mcr.microsoft.com/windows/nanoserver:ltsc2022";
    spec.isolation = "process";

    const auto body = build_create_body(spec);
    ASSERT_TRUE(body.contains("HostConfig"));
    EXPECT_EQ(body["HostConfig"]["Isolation"].get<std::string>(), "process");

    // Unset by default: Linux daemons must never see an Isolation field.
    CreateContainerSpec plain;
    plain.image = "alpine:3.20";
    EXPECT_FALSE(build_create_body(plain).contains("HostConfig"));
}

TEST(ApiMapping, BuildCreateBodyAutoRemove) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.auto_remove = true;

    const auto body = build_create_body(spec);
    ASSERT_TRUE(body.contains("HostConfig"));
    EXPECT_TRUE(body["HostConfig"]["AutoRemove"].get<bool>());

    CreateContainerSpec plain;
    plain.image = "alpine:3.20";
    EXPECT_FALSE(build_create_body(plain).contains("HostConfig"));
}

TEST(ApiMapping, BuildCreateBodyBindMount) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.mounts = {Mount::bind("/host/data", "/data").with_read_only()};

    const auto body = build_create_body(spec);
    ASSERT_TRUE(body.contains("HostConfig"));
    ASSERT_TRUE(body["HostConfig"].contains("Mounts"));
    const auto& mounts = body["HostConfig"]["Mounts"];
    ASSERT_EQ(mounts.size(), 1u);
    EXPECT_EQ(mounts[0]["Type"], "bind");
    EXPECT_EQ(mounts[0]["Source"], "/host/data");
    EXPECT_EQ(mounts[0]["Target"], "/data");
    EXPECT_TRUE(mounts[0]["ReadOnly"].get<bool>());
    EXPECT_FALSE(mounts[0].contains("TmpfsOptions"));
}

TEST(ApiMapping, BuildCreateBodyVolumeMount) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.mounts = {Mount::volume("my-vol", "/var/lib/data")};

    const auto body = build_create_body(spec);
    const auto& mounts = body["HostConfig"]["Mounts"];
    ASSERT_EQ(mounts.size(), 1u);
    EXPECT_EQ(mounts[0]["Type"], "volume");
    EXPECT_EQ(mounts[0]["Source"], "my-vol");
    EXPECT_EQ(mounts[0]["Target"], "/var/lib/data");
    EXPECT_FALSE(mounts[0]["ReadOnly"].get<bool>());
}

TEST(ApiMapping, BuildCreateBodyTmpfsMount) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.mounts = {Mount::tmpfs("/cache").with_tmpfs_size(1048576).with_tmpfs_mode(0700)};

    const auto body = build_create_body(spec);
    const auto& mounts = body["HostConfig"]["Mounts"];
    ASSERT_EQ(mounts.size(), 1u);
    EXPECT_EQ(mounts[0]["Type"], "tmpfs");
    EXPECT_FALSE(mounts[0].contains("Source")); // no source for tmpfs
    EXPECT_EQ(mounts[0]["Target"], "/cache");
    ASSERT_TRUE(mounts[0].contains("TmpfsOptions"));
    EXPECT_EQ(mounts[0]["TmpfsOptions"]["SizeBytes"].get<std::int64_t>(), 1048576);
    EXPECT_EQ(mounts[0]["TmpfsOptions"]["Mode"].get<int>(), 0700);
}

TEST(ApiMapping, BuildCreateBodyNoMountsByDefault) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    const auto body = build_create_body(spec);
    EXPECT_FALSE(body.contains("HostConfig"));
}

TEST(ApiMapping, BuildCreateBodyNetworkMode) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.network = "my-net";

    const auto body = build_create_body(spec);
    ASSERT_TRUE(body.contains("HostConfig"));
    EXPECT_EQ(body["HostConfig"]["NetworkMode"], "my-net");
}

TEST(ApiMapping, BuildCreateBodyNoNetworkModeByDefault) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    const auto body = build_create_body(spec);
    EXPECT_FALSE(body.contains("HostConfig"));
}

TEST(ApiMapping, BuildCreateBodyNetworkAliases) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.network = "netX";
    spec.network_aliases = {"db", "primary"};

    const auto body = build_create_body(spec);
    ASSERT_TRUE(body.contains("NetworkingConfig"));
    const auto& endpoints = body["NetworkingConfig"]["EndpointsConfig"];
    ASSERT_TRUE(endpoints.contains("netX"));
    EXPECT_EQ(endpoints["netX"]["Aliases"], nlohmann::json({"db", "primary"}));

    // Aliases without a target network have nothing to attach to: no-op.
    CreateContainerSpec orphan;
    orphan.image = "alpine:3.20";
    orphan.network_aliases = {"db"};
    EXPECT_FALSE(build_create_body(orphan).contains("NetworkingConfig"));
}

TEST(ApiMapping, BuildCreateBodyHostConfigKnobs) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.memory_bytes = 67108864;
    spec.shm_size_bytes = 33554432;
    spec.ulimits = {Ulimit{"nofile", 1024, 2048}, Ulimit{"nproc", 512, 1024}};
    spec.cap_add = {"NET_ADMIN", "SYS_TIME"};
    spec.cap_drop = {"MKNOD"};
    spec.extra_hosts = {"myhost:1.2.3.4"};

    const auto body = build_create_body(spec);
    ASSERT_TRUE(body.contains("HostConfig"));
    const auto& host = body["HostConfig"];
    EXPECT_EQ(host["Memory"].get<std::int64_t>(), 67108864);
    EXPECT_EQ(host["ShmSize"].get<std::int64_t>(), 33554432);

    ASSERT_TRUE(host.contains("Ulimits"));
    const auto& ulimits = host["Ulimits"];
    ASSERT_EQ(ulimits.size(), 2u);
    EXPECT_EQ(ulimits[0]["Name"], "nofile");
    EXPECT_EQ(ulimits[0]["Soft"].get<std::int64_t>(), 1024);
    EXPECT_EQ(ulimits[0]["Hard"].get<std::int64_t>(), 2048);
    EXPECT_EQ(ulimits[1]["Name"], "nproc");
    EXPECT_EQ(ulimits[1]["Soft"].get<std::int64_t>(), 512);
    EXPECT_EQ(ulimits[1]["Hard"].get<std::int64_t>(), 1024);

    EXPECT_EQ(host["CapAdd"], nlohmann::json({"NET_ADMIN", "SYS_TIME"}));
    EXPECT_EQ(host["CapDrop"], nlohmann::json({"MKNOD"}));
    EXPECT_EQ(host["ExtraHosts"], nlohmann::json({"myhost:1.2.3.4"}));
}

TEST(ApiMapping, BuildNetworkCreateBody) {
    NetworkCreateSpec spec;
    spec.name = "my-net";
    spec.driver = "bridge";
    spec.internal = true;
    spec.attachable = true;
    spec.enable_ipv6 = true;
    spec.subnet = "172.31.250.0/24";
    spec.gateway = "172.31.250.1";
    spec.options = {{"com.docker.network.bridge.name", "br-tc"}};
    spec.labels = {{"k", "v"}};

    const auto body = build_network_create_body(spec);
    EXPECT_EQ(body["Name"], "my-net");
    EXPECT_EQ(body["Driver"], "bridge");
    EXPECT_TRUE(body["Internal"].get<bool>());
    EXPECT_TRUE(body["Attachable"].get<bool>());
    EXPECT_TRUE(body["EnableIPv6"].get<bool>());

    ASSERT_TRUE(body.contains("IPAM"));
    const auto& config = body["IPAM"]["Config"];
    ASSERT_EQ(config.size(), 1u);
    EXPECT_EQ(config[0]["Subnet"], "172.31.250.0/24");
    EXPECT_EQ(config[0]["Gateway"], "172.31.250.1");

    EXPECT_EQ(body["Options"]["com.docker.network.bridge.name"], "br-tc");
    EXPECT_EQ(body["Labels"]["k"], "v");
}

TEST(ApiMapping, BuildNetworkCreateBodyMinimal) {
    NetworkCreateSpec spec;
    spec.name = "bare-net";

    const auto body = build_network_create_body(spec);
    EXPECT_EQ(body["Name"], "bare-net");
    EXPECT_FALSE(body.contains("Driver"));
    EXPECT_FALSE(body.contains("Internal"));
    EXPECT_FALSE(body.contains("Attachable"));
    EXPECT_FALSE(body.contains("EnableIPv6"));
    EXPECT_FALSE(body.contains("IPAM"));
    EXPECT_FALSE(body.contains("Options"));
    EXPECT_FALSE(body.contains("Labels"));
}

TEST(ApiMapping, BuildConnectNetworkBody) {
    const auto bare = build_connect_network_body("abc123", {});
    EXPECT_EQ(bare["Container"], "abc123");
    EXPECT_FALSE(bare.contains("EndpointConfig"));

    const auto with_aliases = build_connect_network_body("abc123", {"cache", "redis"});
    EXPECT_EQ(with_aliases["Container"], "abc123");
    ASSERT_TRUE(with_aliases.contains("EndpointConfig"));
    EXPECT_EQ(with_aliases["EndpointConfig"]["Aliases"],
              (nlohmann::json::array({"cache", "redis"})));
}

TEST(ApiMapping, BuildVolumeCreateBody) {
    VolumeCreateSpec spec;
    spec.name = "my-vol";
    spec.driver = "local";
    spec.driver_opts = {{"type", "tmpfs"}, {"device", "tmpfs"}};
    spec.labels = {{"k", "v"}};

    const auto body = build_volume_create_body(spec);
    EXPECT_EQ(body["Name"], "my-vol");
    EXPECT_EQ(body["Driver"], "local");
    EXPECT_EQ(body["DriverOpts"]["type"], "tmpfs");
    EXPECT_EQ(body["DriverOpts"]["device"], "tmpfs");
    EXPECT_EQ(body["Labels"]["k"], "v");
}

TEST(ApiMapping, BuildVolumeCreateBodyMinimal) {
    VolumeCreateSpec spec;
    spec.name = "bare-vol";

    const auto body = build_volume_create_body(spec);
    EXPECT_EQ(body["Name"], "bare-vol");
    EXPECT_FALSE(body.contains("Driver"));
    EXPECT_FALSE(body.contains("DriverOpts"));
    EXPECT_FALSE(body.contains("Labels"));
}

TEST(ApiMapping, ParseVolumeInspect) {
    const std::string body = R"({
        "Name": "my-vol",
        "Driver": "local",
        "Mountpoint": "/var/lib/docker/volumes/my-vol/_data",
        "Scope": "local",
        "Labels": {"org.testcontainers.managed-by": "testcontainers"},
        "Options": {"type": "tmpfs"}
    })";

    const auto info = parse_volume_inspect(body);
    EXPECT_EQ(info.name, "my-vol");
    EXPECT_EQ(info.driver, "local");
    EXPECT_EQ(info.mountpoint, "/var/lib/docker/volumes/my-vol/_data");
    EXPECT_EQ(info.scope, "local");
    ASSERT_EQ(info.labels.count("org.testcontainers.managed-by"), 1u);
    EXPECT_EQ(info.labels.at("org.testcontainers.managed-by"), "testcontainers");
    ASSERT_EQ(info.options.count("type"), 1u);
    EXPECT_EQ(info.options.at("type"), "tmpfs");
}

TEST(ApiMapping, ParseVolumeInspectNullMaps) {
    // The daemon returns null (not {}) for an unlabelled volume with no options.
    const std::string body = R"({
        "Name": "plain-vol",
        "Driver": "local",
        "Mountpoint": "/var/lib/docker/volumes/plain-vol/_data",
        "Scope": "local",
        "Labels": null,
        "Options": null
    })";

    const auto info = parse_volume_inspect(body);
    EXPECT_EQ(info.name, "plain-vol");
    EXPECT_TRUE(info.labels.empty());
    EXPECT_TRUE(info.options.empty());
}

TEST(ApiMapping, BuildCreateBodyPatchDeepMerges) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.privileged = true;
    spec.create_body_patch = R"({"HostConfig":{"Memory":67108864},"Hostname":"h"})";

    const auto body = build_create_body(spec);
    // The merge is RFC 7386 (deep), so it adds to HostConfig rather than replacing it.
    ASSERT_TRUE(body.contains("HostConfig"));
    EXPECT_TRUE(body["HostConfig"]["Privileged"].get<bool>());
    EXPECT_EQ(body["HostConfig"]["Memory"].get<std::int64_t>(), 67108864);
    EXPECT_EQ(body["Hostname"], "h");
}

TEST(ApiMapping, BuildCreateBodyPatchInvalidThrows) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.create_body_patch = "{not valid json";
    EXPECT_THROW(build_create_body(spec), DockerError);
}

TEST(ApiMapping, BuildCreateQueryEmptyByDefault) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    EXPECT_EQ(build_create_query(spec, test_encode), "");
}

TEST(ApiMapping, BuildCreateQueryName) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.name = "my-container";
    EXPECT_EQ(build_create_query(spec, test_encode), "?name=my-container");
}

TEST(ApiMapping, BuildCreateQueryPlatform) {
    CreateContainerSpec spec;
    spec.image = "mcr.microsoft.com/windows/nanoserver:ltsc2025";
    spec.platform = "windows/amd64";
    // The encoder runs on the value, so the '/' is percent-encoded.
    EXPECT_EQ(build_create_query(spec, test_encode), "?platform=windows%2Famd64");
}

TEST(ApiMapping, BuildCreateQueryNameAndPlatform) {
    CreateContainerSpec spec;
    spec.image = "mcr.microsoft.com/windows/nanoserver:ltsc2025";
    spec.name = "win-box";
    spec.platform = "windows/amd64";
    EXPECT_EQ(build_create_query(spec, test_encode), "?name=win-box&platform=windows%2Famd64");
}

TEST(ApiMapping, ParseServerOs) {
    EXPECT_EQ(parse_server_os(R"({"Version":"29.5.3","Os":"windows","Arch":"amd64"})"), "windows");
    EXPECT_EQ(parse_server_os(R"({"Os":"linux"})"), "linux");
    // Missing Os field defaults to "".
    EXPECT_EQ(parse_server_os(R"({"Version":"24.0.0"})"), "");
}

TEST(ApiMapping, BuildExecCreateBody) {
    const auto body = build_exec_create_body({"echo", "hello-exec"});
    EXPECT_EQ(body["Cmd"], nlohmann::json({"echo", "hello-exec"}));
    EXPECT_TRUE(body["AttachStdout"].get<bool>());
    EXPECT_TRUE(body["AttachStderr"].get<bool>());
}

TEST(ApiMapping, BuildExecCreateBodyDefaultsOmitOptions) {
    // A default-constructed ExecOptions sets Tty=false and omits every optional
    // field (no AttachStdin / Env / WorkingDir / User / Privileged).
    const auto body = build_exec_create_body({"echo", "hi"}, ExecOptions{});
    EXPECT_FALSE(body["Tty"].get<bool>());
    EXPECT_FALSE(body.contains("AttachStdin"));
    EXPECT_FALSE(body.contains("Env"));
    EXPECT_FALSE(body.contains("WorkingDir"));
    EXPECT_FALSE(body.contains("User"));
    EXPECT_FALSE(body.contains("Privileged"));
}

TEST(ApiMapping, BuildExecCreateBodyWithOptions) {
    ExecOptions opts;
    opts.env = {"FOO=bar", "BAZ=qux"};
    opts.working_dir = "/work";
    opts.user = "1000:1000";
    opts.privileged = true;
    opts.tty = true;

    const auto body = build_exec_create_body({"env"}, opts);
    EXPECT_EQ(body["Env"], nlohmann::json({"FOO=bar", "BAZ=qux"}));
    EXPECT_EQ(body["WorkingDir"], "/work");
    EXPECT_EQ(body["User"], "1000:1000");
    EXPECT_TRUE(body["Privileged"].get<bool>());
    EXPECT_TRUE(body["Tty"].get<bool>());
    // stdin not requested -> no AttachStdin.
    EXPECT_FALSE(body.contains("AttachStdin"));
}

TEST(ApiMapping, BuildExecCreateBodyStdinAttaches) {
    ExecOptions opts;
    opts.stdin_data = "ping\n";

    const auto body = build_exec_create_body({"cat"}, opts);
    ASSERT_TRUE(body.contains("AttachStdin"));
    EXPECT_TRUE(body["AttachStdin"].get<bool>());

    // Absent stdin_data emits no AttachStdin field.
    EXPECT_FALSE(build_exec_create_body({"cat"}, ExecOptions{}).contains("AttachStdin"));
}

TEST(ApiMapping, ParseExecExitCode) {
    EXPECT_EQ(parse_exec_exit_code(R"({"ExitCode": 5, "Running": false})"), 5);
    EXPECT_EQ(parse_exec_exit_code(R"({"ExitCode": 0})"), 0);
    // Absent / null ExitCode (exec still running) defaults to 0.
    EXPECT_EQ(parse_exec_exit_code(R"({"Running": true, "ExitCode": null})"), 0);
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

TEST(ApiMapping, ParseInspectTty) {
    const std::string with_tty = R"({
        "Id": "abc123",
        "Config": {"Tty": true},
        "State": {"Status": "running", "Running": true}
    })";
    EXPECT_TRUE(parse_inspect(with_tty).tty);

    // Absent Config / Tty defaults to false.
    const std::string without = R"({
        "Id": "abc123",
        "State": {"Status": "running", "Running": true}
    })";
    EXPECT_FALSE(parse_inspect(without).tty);
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

TEST(ApiMapping, ParseInspectMalformedHostPort) {
    // A daemon should never send these, but a malformed HostPort must not escape
    // as a raw exception AND must not survive as a host_port=0 binding (the
    // IPv4-preferring selection would pick it over a valid IPv6 one): the whole
    // binding is dropped. Valid bindings on the same key are kept.
    const std::string body = R"({
        "Id": "abc123",
        "State": {"Status": "running", "Running": true},
        "NetworkSettings": {"Ports": {
            "6379/tcp": [{"HostIp": "0.0.0.0", "HostPort": "notaport"},
                         {"HostIp": "0.0.0.0", "HostPort": "99999"},
                         {"HostIp": "0.0.0.0", "HostPort": "49153junk"},
                         {"HostIp": "::", "HostPort": "32769"}]
        }}
    })";

    const auto info = parse_inspect(body);
    ASSERT_EQ(info.ports.at("6379/tcp").size(), 1u); // only the valid IPv6 binding
    EXPECT_EQ(info.ports.at("6379/tcp")[0].host_ip, "::");
    EXPECT_EQ(info.ports.at("6379/tcp")[0].host_port, 32769);
}

TEST(ApiMapping, ParseContainerList) {
    const std::string body = R"([
        {"Id": "abc", "Names": ["/x"], "Image": "redis", "State": "running",
         "Labels": {"com.docker.compose.service": "redis",
                    "com.docker.compose.project": "tc123"}}
    ])";

    const auto summaries = parse_container_list(body);
    ASSERT_EQ(summaries.size(), 1u);
    const auto& s = summaries.front();
    EXPECT_EQ(s.id, "abc");
    ASSERT_EQ(s.names.size(), 1u);
    EXPECT_EQ(s.names[0], "/x");
    EXPECT_EQ(s.image, "redis");
    EXPECT_EQ(s.state, "running");
    EXPECT_EQ(s.labels.at("com.docker.compose.service"), "redis");
    EXPECT_EQ(s.labels.at("com.docker.compose.project"), "tc123");
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

TEST(ApiMapping, PullNonStringErrorThrows) {
    // ANY "error" key is the daemon reporting a failed pull (docker's jsonmessage
    // errors on any non-nil error); a non-string payload is dumped, never
    // swallowed — and it must surface as DockerError, not raw json::type_error.
    const std::string stream =
        R"({"status":"Pulling from library/alpine"})"
        "\n"
        R"({"error":{"code":500}})"
        "\n";
    EXPECT_THROW(throw_if_pull_error(stream, "alpine:3.20"), DockerError);
}

TEST(ApiMapping, BuildErrorThrows) {
    const std::string stream =
        R"({"stream":"Step 1/2 : FROM alpine:3.20"})"
        "\n"
        R"({"errorDetail":{"message":"boom"},"error":"boom"})"
        "\n";
    EXPECT_THROW(throw_if_build_error(stream), DockerError);
}

TEST(ApiMapping, BuildSuccessDoesNotThrow) {
    const std::string stream =
        R"({"stream":"Step 1/2 : FROM alpine:3.20"})"
        "\n"
        R"({"stream":"Successfully built abc123"})"
        "\n";
    EXPECT_NO_THROW(throw_if_build_error(stream));
}

TEST(ApiMapping, BuildQueryBasics) {
    BuildOptions options;
    options.tag = "myimg:latest";
    // identity encoder: the query keys/structure are what matter here.
    const auto identity = [](const std::string& v) { return v; };

    const std::string q = build_build_query(options, identity);
    EXPECT_NE(q.find("t=myimg:latest"), std::string::npos);
    EXPECT_NE(q.find("dockerfile=Dockerfile"), std::string::npos);
    // Always present: without forcerm the legacy builder LEAKS the failed
    // step's intermediate (unlabelled, hence un-reapable) container.
    EXPECT_NE(q.find("forcerm=1"), std::string::npos);
    // Off-by-default flags / unset target are omitted.
    EXPECT_EQ(q.find("nocache="), std::string::npos);
    EXPECT_EQ(q.find("pull="), std::string::npos);
    EXPECT_EQ(q.find("target="), std::string::npos);

    options.no_cache = true;
    options.pull = true;
    options.target = "builder";
    const std::string q2 = build_build_query(options, identity);
    EXPECT_NE(q2.find("nocache=1"), std::string::npos);
    EXPECT_NE(q2.find("pull=1"), std::string::npos);
    EXPECT_NE(q2.find("target=builder"), std::string::npos);
}

TEST(ApiMapping, BuildQueryBuildArgs) {
    BuildOptions options;
    options.tag = "myimg:latest";
    options.build_args = {{"VERSION", "1.2"}, {"FLAG", "on"}};

    // Use the real test_encode (percent-encodes only '/') is insufficient here;
    // instead capture the raw buildargs value and assert it parses as the JSON map.
    const std::string q = build_build_query(options, [](const std::string& v) { return v; });
    const std::size_t pos = q.find("buildargs=");
    ASSERT_NE(pos, std::string::npos);
    std::string value = q.substr(pos + std::string("buildargs=").size());
    if (const std::size_t amp = value.find('&'); amp != std::string::npos) {
        value = value.substr(0, amp);
    }
    const auto parsed = nlohmann::json::parse(value);
    EXPECT_EQ(parsed["VERSION"], "1.2");
    EXPECT_EQ(parsed["FLAG"], "on");
}

TEST(ApiMapping, ExpectStringFieldExtracts) {
    EXPECT_EQ(expect_string_field(R"({"Id":"abc123","Warnings":[]})", "Id", "ctx"), "abc123");
    EXPECT_EQ(expect_string_field(R"({"Name":"vol1"})", "Name", "ctx"), "vol1");
}

TEST(ApiMapping, ExpectStringFieldWrapsFailures) {
    // Malformed JSON (e.g. an HTML error page through a 200), a missing field,
    // and a wrong-typed field must all surface as DockerError with the context —
    // never as raw nlohmann exceptions.
    for (const char* body : {"<html>502</html>", R"({"NotId":"x"})", R"({"Id":42})"}) {
        try {
            (void)expect_string_field(body, "Id", "create_container('img')");
            FAIL() << "expected DockerError for body: " << body;
        } catch (const DockerError& e) {
            EXPECT_NE(std::string(e.what()).find("create_container('img')"), std::string::npos);
        }
    }
}
