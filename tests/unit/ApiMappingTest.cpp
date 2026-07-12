#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "docker/ApiMapping.hpp"
#include "testcontainers/Device.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/ExecOptions.hpp"
#include "testcontainers/Healthcheck.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/RestartPolicy.hpp"
#include "testcontainers/TtySize.hpp"
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
//   ApiMapping.BuildCreateBodyStaticIpv4 - a network plus static_ipv4 maps to the endpoint's IPAMConfig.IPv4Address (no Aliases key without aliases; combines with aliases; no-op without a network).
//   ApiMapping.BuildCreateBodyHostConfigKnobs - memory, shm size, ulimits, cap add/drop, and extra hosts map to their HostConfig fields.
//   ApiMapping.BuildCreateBodyCpuPidsKnobs - nano_cpus, cpuset_cpus, and pids_limit map to HostConfig NanoCpus/CpusetCpus/PidsLimit.
//   ApiMapping.BuildCreateBodyRestartPolicy - each RestartPolicy factory maps to HostConfig.RestartPolicy with Docker's Name spelling and the retry count.
//   ApiMapping.BuildCreateBodyDnsAndSysctls - dns servers/search/options map to HostConfig Dns/DnsSearch/DnsOptions arrays and sysctl pairs to the Sysctls object.
//   ApiMapping.BuildCreateBodyDevices - devices map to HostConfig.Devices entries with PathOnHost/PathInContainer/CgroupPermissions (default "rwm").
//   ApiMapping.BuildNetworkCreateBody - a full NetworkCreateSpec maps to Name, Driver, Internal, Attachable, EnableIPv6, Options, Labels, and IPAM.Config: the subnet/gateway shorthand pool first, then each ipam_pools entry (Subnet/IPRange/Gateway/AuxiliaryAddresses, empty fields omitted).
//   ApiMapping.BuildNetworkCreateBodyMinimal - a NetworkCreateSpec with only a name emits just Name and no Driver/IPAM/flags.
//   ApiMapping.BuildConnectNetworkBody - the connect body carries Container, adding EndpointConfig.Aliases only when aliases are given.
//   ApiMapping.BuildVolumeCreateBody - a full VolumeCreateSpec maps to Name, Driver, DriverOpts, and Labels.
//   ApiMapping.BuildVolumeCreateBodyMinimal - a VolumeCreateSpec with only a name emits just Name and no Driver/DriverOpts/Labels.
//   ApiMapping.ParseVolumeInspect - GET /volumes/{name} JSON parses Name/Driver/Mountpoint/Scope and the Labels/Options maps.
//   ApiMapping.ParseVolumeInspectNullMaps - null Labels/Options parse into empty maps.
//   ApiMapping.ParseVolumeList - the GET /volumes {"Volumes":[...]} wrapper parses into one VolumeInspect per object entry (non-object entries skipped); null Volumes / a null body parse as an empty list.
//   ApiMapping.ParseVolumePrune - POST /volumes/prune JSON parses VolumesDeleted + SpaceReclaimed; null/absent fields parse as empty/0.
//   ApiMapping.ParseNetworkInspect - GET /networks/{id} JSON parses id/name/driver/scope, the Internal/Attachable/EnableIPv6 flags, IPAM pools (Subnet/Gateway/IPRange + name-sorted AuxiliaryAddresses), Options/Labels maps, and the Containers endpoint map.
//   ApiMapping.ParseNetworkInspectNullsAndGarbage - null/absent Labels/Options/Containers/IPAM.Config parse into empty containers with false flags; a non-JSON body throws DockerError.
//   ApiMapping.ParseNetworkList - a GET /networks array parses into one NetworkInspect per object entry (non-object entries skipped); a null body parses as an empty list.
//   ApiMapping.ParseImageInspect - GET /images/{ref}/json JSON parses id/tags/digests/created/arch/os/size and the Config fields (labels, env, cmd, entrypoint, exposed ports, workdir, user).
//   ApiMapping.ParseImageInspectNullsAndGarbage - null RepoTags (dangling image) and null Config members parse into empty containers, absent Size becomes 0; a non-JSON body throws DockerError.
//   ApiMapping.BuildCreateBodyPatchDeepMerges - create_body_patch deep-merges into the body, keeping existing fields while adding nested and top-level ones.
//   ApiMapping.BuildCreateBodyPatchInvalidThrows - an invalid-JSON create_body_patch makes build_create_body throw DockerError.
//   ApiMapping.BuildCreateQueryEmptyByDefault - a spec with neither name nor platform yields an empty create query.
//   ApiMapping.BuildCreateQueryName - a spec with only a name yields "?name=<encoded>".
//   ApiMapping.BuildCreateQueryPlatform - a spec with a platform yields "?platform=<encoded>" (slash percent-encoded).
//   ApiMapping.BuildCreateQueryNameAndPlatform - a spec with both name and platform joins them with '&'.
//   ApiMapping.ParseServerOs - GET /version JSON parses the Os field ("windows"/"linux"), defaulting to "" when absent.
//   ApiMapping.ParseRfc3339KnownInstants - parse_rfc3339 matches known epoch offsets: the epoch itself, a known date, fractional seconds truncated, ±HH:MM offsets applied, and Go's zero time parsing (far before the epoch).
//   ApiMapping.ParseRfc3339RejectsMalformed - missing zone, bad separators, short fields, out-of-range values, an empty fraction, and trailing garbage all yield nullopt.
//   ApiMapping.BuildExecCreateBody - the exec-create body carries Cmd and AttachStdout/AttachStderr set true.
//   ApiMapping.BuildExecCreateBodyDefaultsOmitOptions - with default ExecOptions the body sets Tty=false and omits AttachStdin/Env/WorkingDir/User/Privileged.
//   ApiMapping.BuildExecCreateBodyWithOptions - env/working_dir/user/privileged/tty map to Env/WorkingDir/User/Privileged/Tty.
//   ApiMapping.BuildExecCreateBodyStdinAttaches - a set stdin_data adds AttachStdin=true (and is absent otherwise).
//   ApiMapping.BuildExecCreateBodyDetachAttachesNothing - detach=true omits every Attach* field (a detached exec streams nothing back), still emitting Cmd and Tty.
//   ApiMapping.BuildExecCreateBodyConsoleSize - console_size emits ConsoleSize as the [height, width] array (rows first) and is omitted when unset.
//   ApiMapping.ParseExecStatus - exec-inspect JSON parses Running + ExitCode; a running exec's null ExitCode reads as absent (never a type error), a finished one carries its code.
//   ApiMapping.ParseInspectExtractsStateAndPorts - inspect JSON parses into id, name, running state, and per-port host bindings (null becomes empty).
//   ApiMapping.ParseInspectTty - inspect JSON with Config.Tty=true parses into ContainerInspect.tty (false when absent).
//   ApiMapping.ParseInspectHealthStatus - inspect JSON with State.Health.Status fills health_status.
//   ApiMapping.ParseInspectNoHealthStatus - inspect JSON without State.Health yields a nullopt health_status.
//   ApiMapping.ParseInspectMalformedHostPort - a non-numeric, out-of-range, or trailing-garbage HostPort drops that binding (valid siblings survive) instead of throwing or surviving as port 0.
//   ApiMapping.ParseInspectHostConfigEcho - inspect JSON's HostConfig echo parses into host_config: memory/shm/nano-cpus/cpuset, pids limit, restart policy, dns triple, sysctls, and devices.
//   ApiMapping.ParseInspectHostConfigAbsentAndNulls - a missing HostConfig object, null members, and a null PidsLimit parse into the zero state (0 / "" / empty / nullopt) instead of throwing.
//   ApiMapping.ParseContainerList - a /containers/json array parses into ContainerSummary entries with id, names, image, state, and labels.
//   ApiMapping.SplitImage - "name[:tag]" splits into name and tag, defaulting to "latest" (bare trailing ':' included) and handling a registry host:port; a digest reference splits at the '@' with the "sha256:..." digest as the tag.
//   ApiMapping.JoinImage - join_image re-attaches a tag with ':' and a digest with '@' (round-trips every split_image shape).
//   ApiMapping.PullErrorThrows - a pull progress stream containing an error entry throws DockerError.
//   ApiMapping.PullSuccessDoesNotThrow - a clean pull progress stream does not throw.
//   ApiMapping.PullNonStringErrorThrows - a pull stream entry whose "error" is not a string still throws DockerError (dumped payload), never a raw json type_error.
//   ApiMapping.BuildScannerEmitsStreamLines - BuildStreamScanner reassembles lines split across feed() chunks and hands each "stream" payload to the consumer in order.
//   ApiMapping.BuildScannerErrorThrowsWithOutputTail - finish() after an error line throws DockerError carrying the daemon message, the preceding step output, and the tag as resource_id.
//   ApiMapping.BuildScannerErrorDetailWithoutError - an errorDetail-only line still records the failure.
//   ApiMapping.BuildScannerTrailingLineWithoutNewline - a final error line with no trailing newline is scanned by finish().
//   ApiMapping.BuildScannerSuccessAndJunkLines - blank and non-JSON lines are ignored and a clean stream does not throw (no consumer attached).
//   ApiMapping.BuildScannerTailIsBounded - the error message keeps only the LAST few KB of step output: bounded overall and ending with the most recent line.
//   ApiMapping.BuildQueryBasics - build_build_query always emits t, dockerfile and forcerm=1 (a failed build's intermediate container must not leak) and includes nocache/pull/target only when set.
//   ApiMapping.BuildQueryBuildArgs - a build_arg yields a buildargs= value that URL-decodes to the JSON map.
//   ApiMapping.BuildQueryLabels - labels yield a labels= value that parses back to the JSON map; no labels means no labels= key.
//   ApiMapping.ExpectStringFieldExtracts - expect_string_field returns the named top-level string field.
//   ApiMapping.ExpectStringFieldWrapsFailures - a malformed body, a missing field, and a non-string field all throw DockerError carrying the context, never raw nlohmann exceptions.

using namespace testcontainers;
using namespace std::chrono_literals;
using testcontainers::docker::build_build_query;
using testcontainers::docker::build_connect_network_body;
using testcontainers::docker::build_create_body;
using testcontainers::docker::build_create_query;
using testcontainers::docker::build_exec_create_body;
using testcontainers::docker::build_network_create_body;
using testcontainers::docker::build_volume_create_body;
using testcontainers::docker::BuildOptions;
using testcontainers::docker::BuildStreamScanner;
using testcontainers::docker::expect_string_field;
using testcontainers::docker::parse_container_list;
using testcontainers::docker::parse_exec_status;
using testcontainers::docker::parse_image_inspect;
using testcontainers::docker::parse_inspect;
using testcontainers::docker::parse_network_inspect;
using testcontainers::docker::parse_network_list;
using testcontainers::docker::parse_rfc3339;
using testcontainers::docker::parse_server_os;
using testcontainers::docker::parse_volume_inspect;
using testcontainers::docker::parse_volume_list;
using testcontainers::docker::parse_volume_prune;
using testcontainers::docker::split_image;
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
    EXPECT_EQ(health["Interval"].get<std::int64_t>(), std::chrono::nanoseconds(500ms).count());
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

TEST(ApiMapping, BuildCreateBodyStaticIpv4) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.network = "netX";
    spec.static_ipv4 = "10.246.200.11";

    const auto body = build_create_body(spec);
    ASSERT_TRUE(body.contains("NetworkingConfig"));
    const auto& endpoint = body["NetworkingConfig"]["EndpointsConfig"]["netX"];
    EXPECT_EQ(endpoint["IPAMConfig"]["IPv4Address"], "10.246.200.11");
    EXPECT_FALSE(endpoint.contains("Aliases")); // no aliases were set

    // A static IP and aliases share the same endpoint object.
    spec.network_aliases = {"db"};
    const auto both = build_create_body(spec);
    const auto& ep = both["NetworkingConfig"]["EndpointsConfig"]["netX"];
    EXPECT_EQ(ep["IPAMConfig"]["IPv4Address"], "10.246.200.11");
    EXPECT_EQ(ep["Aliases"], nlohmann::json({"db"}));

    // A static IP without a target network has no endpoint to pin: no-op.
    CreateContainerSpec orphan;
    orphan.image = "alpine:3.20";
    orphan.static_ipv4 = "10.246.200.11";
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

TEST(ApiMapping, BuildCreateBodyCpuPidsKnobs) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.nano_cpus = 1500000000; // 1.5 CPUs
    spec.cpuset_cpus = "0-2,7";
    spec.pids_limit = 64;

    const auto body = build_create_body(spec);
    ASSERT_TRUE(body.contains("HostConfig"));
    const auto& host = body["HostConfig"];
    EXPECT_EQ(host["NanoCpus"].get<std::int64_t>(), 1500000000);
    EXPECT_EQ(host["CpusetCpus"], "0-2,7");
    EXPECT_EQ(host["PidsLimit"].get<std::int64_t>(), 64);
}

TEST(ApiMapping, BuildCreateBodyRestartPolicy) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.restart_policy = RestartPolicy::on_failure(3);

    const auto body = build_create_body(spec);
    ASSERT_TRUE(body.contains("HostConfig"));
    const auto& policy = body["HostConfig"]["RestartPolicy"];
    EXPECT_EQ(policy["Name"], "on-failure");
    EXPECT_EQ(policy["MaximumRetryCount"].get<int>(), 3);

    // Docker's Name strings are hyphenated; pin each factory's spelling.
    spec.restart_policy = RestartPolicy::always();
    EXPECT_EQ(build_create_body(spec)["HostConfig"]["RestartPolicy"]["Name"], "always");
    spec.restart_policy = RestartPolicy::unless_stopped();
    const auto body_unless = build_create_body(spec);
    EXPECT_EQ(body_unless["HostConfig"]["RestartPolicy"]["Name"], "unless-stopped");
    EXPECT_EQ(body_unless["HostConfig"]["RestartPolicy"]["MaximumRetryCount"].get<int>(), 0);
}

TEST(ApiMapping, BuildCreateBodyDnsAndSysctls) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.dns_servers = {"192.0.2.53", "192.0.2.54"};
    spec.dns_search = {"svc.test.internal"};
    spec.dns_options = {"ndots:2"};
    spec.sysctls = {{"net.ipv4.ip_unprivileged_port_start", "1024"},
                    {"net.ipv4.ping_group_range", "0 2147483647"}};

    const auto body = build_create_body(spec);
    ASSERT_TRUE(body.contains("HostConfig"));
    const auto& host = body["HostConfig"];
    EXPECT_EQ(host["Dns"], nlohmann::json({"192.0.2.53", "192.0.2.54"}));
    EXPECT_EQ(host["DnsSearch"], nlohmann::json({"svc.test.internal"}));
    EXPECT_EQ(host["DnsOptions"], nlohmann::json({"ndots:2"}));
    EXPECT_EQ(host["Sysctls"]["net.ipv4.ip_unprivileged_port_start"], "1024");
    EXPECT_EQ(host["Sysctls"]["net.ipv4.ping_group_range"], "0 2147483647");
}

TEST(ApiMapping, BuildCreateBodyDevices) {
    CreateContainerSpec spec;
    spec.image = "alpine:3.20";
    spec.devices = {Device{"/dev/fuse", "/dev/tc-fuse", "rw"},
                    Device{"/dev/net/tun", "/dev/net/tun"}}; // permissions default to "rwm"

    const auto body = build_create_body(spec);
    ASSERT_TRUE(body.contains("HostConfig"));
    const auto& devices = body["HostConfig"]["Devices"];
    ASSERT_EQ(devices.size(), 2u);
    EXPECT_EQ(devices[0]["PathOnHost"], "/dev/fuse");
    EXPECT_EQ(devices[0]["PathInContainer"], "/dev/tc-fuse");
    EXPECT_EQ(devices[0]["CgroupPermissions"], "rw");
    EXPECT_EQ(devices[1]["PathOnHost"], "/dev/net/tun");
    EXPECT_EQ(devices[1]["PathInContainer"], "/dev/net/tun");
    EXPECT_EQ(devices[1]["CgroupPermissions"], "rwm");
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
    NetworkIpamPool full_pool;
    full_pool.subnet = "10.77.0.0/16";
    full_pool.ip_range = "10.77.128.0/17";
    full_pool.gateway = "10.77.0.1";
    full_pool.aux_addresses = {{"router", "10.77.0.2"}, {"printer", "10.77.0.3"}};
    NetworkIpamPool bare_pool;
    bare_pool.subnet = "fd00:beef::/64";
    spec.ipam_pools = {full_pool, bare_pool};
    spec.options = {{"com.docker.network.bridge.name", "br-tc"}};
    spec.labels = {{"k", "v"}};

    const auto body = build_network_create_body(spec);
    EXPECT_EQ(body["Name"], "my-net");
    EXPECT_EQ(body["Driver"], "bridge");
    EXPECT_TRUE(body["Internal"].get<bool>());
    EXPECT_TRUE(body["Attachable"].get<bool>());
    EXPECT_TRUE(body["EnableIPv6"].get<bool>());

    // The shorthand subnet/gateway pool leads; ipam_pools follow in order.
    ASSERT_TRUE(body.contains("IPAM"));
    const auto& config = body["IPAM"]["Config"];
    ASSERT_EQ(config.size(), 3u);
    EXPECT_EQ(config[0]["Subnet"], "172.31.250.0/24");
    EXPECT_EQ(config[0]["Gateway"], "172.31.250.1");
    EXPECT_EQ(config[1]["Subnet"], "10.77.0.0/16");
    EXPECT_EQ(config[1]["IPRange"], "10.77.128.0/17");
    EXPECT_EQ(config[1]["Gateway"], "10.77.0.1");
    EXPECT_EQ(config[1]["AuxiliaryAddresses"]["router"], "10.77.0.2");
    EXPECT_EQ(config[1]["AuxiliaryAddresses"]["printer"], "10.77.0.3");
    // Empty pool fields are omitted, not emitted as "".
    EXPECT_EQ(config[2]["Subnet"], "fd00:beef::/64");
    EXPECT_FALSE(config[2].contains("IPRange"));
    EXPECT_FALSE(config[2].contains("Gateway"));
    EXPECT_FALSE(config[2].contains("AuxiliaryAddresses"));

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

TEST(ApiMapping, ParseVolumeList) {
    // GET /volumes wraps its array (unlike GET /networks): each entry is the
    // same Volume shape as the single inspect.
    const std::string body = R"({
        "Volumes": [
            {"Name": "vol-a", "Driver": "local",
             "Mountpoint": "/var/lib/docker/volumes/vol-a/_data", "Scope": "local",
             "Labels": {"tc-mark": "yes"}},
            {"Name": "vol-b", "Driver": "local", "Labels": null},
            "not-an-object"
        ],
        "Warnings": null
    })";

    const auto volumes = parse_volume_list(body);
    ASSERT_EQ(volumes.size(), 2u); // the non-object entry is skipped
    EXPECT_EQ(volumes[0].name, "vol-a");
    EXPECT_EQ(volumes[0].driver, "local");
    EXPECT_EQ(volumes[0].labels.at("tc-mark"), "yes");
    EXPECT_EQ(volumes[1].name, "vol-b");
    EXPECT_TRUE(volumes[1].labels.empty());

    // A null Volumes field (the daemon's "no volumes"), a null body, and a
    // bare-array body (no wrapper) all parse as an empty list, not a crash.
    EXPECT_TRUE(parse_volume_list(R"({"Volumes": null, "Warnings": null})").empty());
    EXPECT_TRUE(parse_volume_list("null").empty());
    EXPECT_TRUE(parse_volume_list("[]").empty());
}

TEST(ApiMapping, ParseVolumePrune) {
    const auto full =
        parse_volume_prune(R"({"VolumesDeleted": ["vol-a", "vol-b"], "SpaceReclaimed": 4096})");
    ASSERT_EQ(full.deleted.size(), 2u);
    EXPECT_EQ(full.deleted[0], "vol-a");
    EXPECT_EQ(full.deleted[1], "vol-b");
    EXPECT_EQ(full.space_reclaimed, 4096);

    // Nothing pruned: the daemon may emit null instead of [] (and older
    // daemons omitted fields entirely) — both fields tolerate null AND absent.
    const auto empty = parse_volume_prune(R"({"VolumesDeleted": null, "SpaceReclaimed": null})");
    EXPECT_TRUE(empty.deleted.empty());
    EXPECT_EQ(empty.space_reclaimed, 0);
    EXPECT_TRUE(parse_volume_prune("{}").deleted.empty());
    EXPECT_EQ(parse_volume_prune("{}").space_reclaimed, 0);
}

TEST(ApiMapping, ParseNetworkInspect) {
    // A representative GET /networks/{id} body: flags on, two IPAM pools (the
    // second IPv6-only, i.e. no Gateway), driver options, labels, and one
    // attached container.
    const std::string body = R"({
        "Name": "tc-net",
        "Id": "0f6bnet",
        "Created": "2026-07-08T10:00:00Z",
        "Scope": "local",
        "Driver": "bridge",
        "EnableIPv6": true,
        "IPAM": {
            "Driver": "default",
            "Config": [
                {"Subnet": "172.31.250.0/24", "Gateway": "172.31.250.1",
                 "IPRange": "172.31.250.128/25",
                 "AuxiliaryAddresses": {"router": "172.31.250.2", "printer": "172.31.250.3"}},
                {"Subnet": "fd00:beef::/64"}
            ]
        },
        "Internal": true,
        "Attachable": true,
        "Containers": {
            "abc123": {
                "Name": "my-svc",
                "EndpointID": "ep-1",
                "MacAddress": "02:42:ac:1f:fa:02",
                "IPv4Address": "172.31.250.2/24",
                "IPv6Address": ""
            }
        },
        "Options": {"com.docker.network.bridge.name": "br-tc"},
        "Labels": {"org.testcontainers.session-id": "s1"}
    })";

    const auto info = parse_network_inspect(body);
    EXPECT_EQ(info.id, "0f6bnet");
    EXPECT_EQ(info.name, "tc-net");
    EXPECT_EQ(info.driver, "bridge");
    EXPECT_EQ(info.scope, "local");
    EXPECT_TRUE(info.internal);
    EXPECT_TRUE(info.attachable);
    EXPECT_TRUE(info.enable_ipv6);

    ASSERT_EQ(info.ipam_pools.size(), 2u);
    EXPECT_EQ(info.ipam_pools[0].subnet, "172.31.250.0/24");
    EXPECT_EQ(info.ipam_pools[0].gateway, "172.31.250.1");
    EXPECT_EQ(info.ipam_pools[0].ip_range, "172.31.250.128/25");
    // Aux addresses come back sorted by name (nlohmann object key order).
    ASSERT_EQ(info.ipam_pools[0].aux_addresses.size(), 2u);
    EXPECT_EQ(info.ipam_pools[0].aux_addresses[0],
              (std::pair<std::string, std::string>{"printer", "172.31.250.3"}));
    EXPECT_EQ(info.ipam_pools[0].aux_addresses[1],
              (std::pair<std::string, std::string>{"router", "172.31.250.2"}));
    EXPECT_EQ(info.ipam_pools[1].subnet, "fd00:beef::/64");
    EXPECT_TRUE(info.ipam_pools[1].gateway.empty());
    EXPECT_TRUE(info.ipam_pools[1].ip_range.empty());
    EXPECT_TRUE(info.ipam_pools[1].aux_addresses.empty());

    ASSERT_EQ(info.options.count("com.docker.network.bridge.name"), 1u);
    EXPECT_EQ(info.options.at("com.docker.network.bridge.name"), "br-tc");
    ASSERT_EQ(info.labels.count("org.testcontainers.session-id"), 1u);
    EXPECT_EQ(info.labels.at("org.testcontainers.session-id"), "s1");

    ASSERT_EQ(info.containers.count("abc123"), 1u);
    const auto& endpoint = info.containers.at("abc123");
    EXPECT_EQ(endpoint.name, "my-svc");
    EXPECT_EQ(endpoint.endpoint_id, "ep-1");
    EXPECT_EQ(endpoint.mac_address, "02:42:ac:1f:fa:02");
    EXPECT_EQ(endpoint.ipv4_address, "172.31.250.2/24");
    EXPECT_TRUE(endpoint.ipv6_address.empty());
}

TEST(ApiMapping, ParseNetworkInspectNullsAndGarbage) {
    // The daemon emits null (not {} / []) for empty maps, and IPAM.Config can
    // be null on some driver/daemon combinations.
    const std::string body = R"({
        "Name": "bare-net",
        "Id": "idnet",
        "Scope": "local",
        "Driver": "bridge",
        "IPAM": {"Driver": "default", "Config": null},
        "Containers": null,
        "Options": null,
        "Labels": null
    })";

    const auto info = parse_network_inspect(body);
    EXPECT_EQ(info.id, "idnet");
    EXPECT_EQ(info.name, "bare-net");
    EXPECT_FALSE(info.internal);
    EXPECT_FALSE(info.attachable);
    EXPECT_FALSE(info.enable_ipv6);
    EXPECT_TRUE(info.ipam_pools.empty());
    EXPECT_TRUE(info.containers.empty());
    EXPECT_TRUE(info.options.empty());
    EXPECT_TRUE(info.labels.empty());

    // A proxy's HTML smuggled through a 200 must surface as DockerError, not a
    // raw nlohmann exception.
    EXPECT_THROW(parse_network_inspect("<html>oops</html>"), DockerError);
}

TEST(ApiMapping, ParseNetworkList) {
    // GET /networks returns an array of the same NetworkResource shape as the
    // single inspect (list responses leave Containers unpopulated).
    const std::string body = R"([
        {"Name": "tc-a", "Id": "id-a", "Driver": "bridge", "Scope": "local",
         "IPAM": {"Config": [{"Subnet": "172.31.250.0/24"}]},
         "Labels": {"org.testcontainers.reuse.hash": "abcd"}},
        {"Name": "tc-b", "Id": "id-b", "Driver": "nat", "Labels": null},
        "not-an-object"
    ])";

    const auto networks = parse_network_list(body);
    ASSERT_EQ(networks.size(), 2u); // the non-object entry is skipped
    EXPECT_EQ(networks[0].name, "tc-a");
    EXPECT_EQ(networks[0].id, "id-a");
    EXPECT_EQ(networks[0].driver, "bridge");
    ASSERT_EQ(networks[0].ipam_pools.size(), 1u);
    EXPECT_EQ(networks[0].ipam_pools[0].subnet, "172.31.250.0/24");
    EXPECT_EQ(networks[0].labels.at("org.testcontainers.reuse.hash"), "abcd");
    EXPECT_EQ(networks[1].name, "tc-b");
    EXPECT_TRUE(networks[1].labels.empty());

    // Null / non-array bodies parse as an empty list, not a crash.
    EXPECT_TRUE(parse_network_list("null").empty());
}

TEST(ApiMapping, ParseImageInspect) {
    // A representative GET /images/{ref}/json body (fields the struct models).
    const std::string body = R"({
        "Id": "sha256:abcd1234",
        "RepoTags": ["redis:7.2", "redis:latest"],
        "RepoDigests": ["redis@sha256:beef"],
        "Created": "2026-01-02T03:04:05Z",
        "Architecture": "amd64",
        "Os": "linux",
        "Size": 123456789,
        "Config": {
            "Env": ["PATH=/usr/local/bin", "REDIS_VERSION=7.2"],
            "Cmd": ["redis-server"],
            "Entrypoint": ["docker-entrypoint.sh"],
            "ExposedPorts": {"6379/tcp": {}},
            "Labels": {"maintainer": "someone"},
            "WorkingDir": "/data",
            "User": "redis"
        }
    })";

    const auto info = parse_image_inspect(body);
    EXPECT_EQ(info.id, "sha256:abcd1234");
    ASSERT_EQ(info.repo_tags.size(), 2u);
    EXPECT_EQ(info.repo_tags[0], "redis:7.2");
    ASSERT_EQ(info.repo_digests.size(), 1u);
    EXPECT_EQ(info.repo_digests[0], "redis@sha256:beef");
    EXPECT_EQ(info.created, "2026-01-02T03:04:05Z");
    EXPECT_EQ(info.architecture, "amd64");
    EXPECT_EQ(info.os, "linux");
    EXPECT_EQ(info.size, 123456789);

    ASSERT_EQ(info.env.size(), 2u);
    EXPECT_EQ(info.env[1], "REDIS_VERSION=7.2");
    ASSERT_EQ(info.cmd.size(), 1u);
    EXPECT_EQ(info.cmd[0], "redis-server");
    ASSERT_EQ(info.entrypoint.size(), 1u);
    EXPECT_EQ(info.entrypoint[0], "docker-entrypoint.sh");
    ASSERT_EQ(info.exposed_ports.size(), 1u);
    EXPECT_EQ(info.exposed_ports[0], "6379/tcp");
    ASSERT_EQ(info.labels.count("maintainer"), 1u);
    EXPECT_EQ(info.labels.at("maintainer"), "someone");
    EXPECT_EQ(info.working_dir, "/data");
    EXPECT_EQ(info.user, "redis");
}

TEST(ApiMapping, ParseImageInspectNullsAndGarbage) {
    // A dangling image has null RepoTags; a minimal image config is full of
    // nulls. ExposedPorts and Size are absent entirely.
    const std::string body = R"({
        "Id": "sha256:ff00",
        "RepoTags": null,
        "RepoDigests": null,
        "Config": {
            "Env": null,
            "Cmd": null,
            "Entrypoint": null,
            "Labels": null
        }
    })";

    const auto info = parse_image_inspect(body);
    EXPECT_EQ(info.id, "sha256:ff00");
    EXPECT_TRUE(info.repo_tags.empty());
    EXPECT_TRUE(info.repo_digests.empty());
    EXPECT_TRUE(info.created.empty());
    EXPECT_EQ(info.size, 0);
    EXPECT_TRUE(info.env.empty());
    EXPECT_TRUE(info.cmd.empty());
    EXPECT_TRUE(info.entrypoint.empty());
    EXPECT_TRUE(info.exposed_ports.empty());
    EXPECT_TRUE(info.labels.empty());

    // Garbage through a 200 must surface as DockerError, not a raw nlohmann
    // exception.
    EXPECT_THROW(parse_image_inspect("not json"), DockerError);
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

TEST(ApiMapping, ParseRfc3339KnownInstants) {
    // The seconds-resolution rep is read directly — converting through the
    // clock's native duration would overflow the extreme vectors below on
    // nanosecond-clock stdlibs (the exact bug the return type avoids).
    const auto epoch_seconds = [](const std::string& text) -> std::optional<std::int64_t> {
        const auto parsed = parse_rfc3339(text);
        if (!parsed) {
            return std::nullopt;
        }
        return parsed->time_since_epoch().count();
    };

    EXPECT_EQ(epoch_seconds("1970-01-01T00:00:00Z"), 0);
    // date -u -d '2024-01-15T10:30:00Z' +%s
    EXPECT_EQ(epoch_seconds("2024-01-15T10:30:00Z"), 1705314600);
    // Fractional seconds truncate (the daemon emits nanoseconds); 'z' works.
    EXPECT_EQ(epoch_seconds("2024-01-15T10:30:00.999999999Z"), 1705314600);
    EXPECT_EQ(epoch_seconds("2024-01-15t10:30:00z"), 1705314600);
    // A +02:00 civil time is two hours EARLIER as an instant; -05:30 later.
    EXPECT_EQ(epoch_seconds("2024-01-15T10:30:00+02:00"), 1705314600 - 7200);
    EXPECT_EQ(epoch_seconds("2024-01-15T10:30:00-05:30"), 1705314600 + 19800);
    // Go's zero time (an image with no Created) parses to far before the
    // epoch — an age check then always calls it stale, which is the point.
    const auto go_zero = epoch_seconds("0001-01-01T00:00:00Z");
    ASSERT_TRUE(go_zero.has_value());
    EXPECT_LT(*go_zero, -60000000000LL); // ~1900 years before 1970
}

TEST(ApiMapping, ParseRfc3339RejectsMalformed) {
    for (const char* text : {
             "",                             // empty
             "2024-01-15T10:30:00",          // zone designator required
             "2024-01-15 10:30:00Z",         // ' ' is not a date/time separator
             "2024/01/15T10:30:00Z",         // wrong date separators
             "2024-1-15T10:30:00Z",          // fields are fixed-width
             "2024-13-15T10:30:00Z",         // month out of range
             "2024-01-15T24:30:00Z",         // hour out of range
             "2024-01-15T10:30:00.Z",        // empty fraction
             "2024-01-15T10:30:00+02",       // offset needs HH:MM
             "2024-01-15T10:30:00Z-trailer", // trailing garbage
             "not a timestamp",
         }) {
        EXPECT_FALSE(parse_rfc3339(text).has_value()) << text;
    }
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

TEST(ApiMapping, BuildExecCreateBodyDetachAttachesNothing) {
    // A detached exec streams nothing back: no Attach* field at all, even with
    // stdin_data set (the detach+stdin combination is rejected before the body
    // is ever built; the pure mapping still must not attach). Detach itself is
    // a START-body field, so it does not appear here either.
    ExecOptions opts;
    opts.detach = true;
    opts.stdin_data = "ping\n";

    const auto body = build_exec_create_body({"sleep", "30"}, opts);
    EXPECT_FALSE(body.contains("AttachStdout"));
    EXPECT_FALSE(body.contains("AttachStderr"));
    EXPECT_FALSE(body.contains("AttachStdin"));
    EXPECT_FALSE(body.contains("Detach"));
    // Cmd / Tty are emitted as usual (`docker exec -d -t` is a legal combo).
    EXPECT_EQ(body["Cmd"], nlohmann::json({"sleep", "30"}));
    EXPECT_FALSE(body["Tty"].get<bool>());
}

TEST(ApiMapping, BuildExecCreateBodyConsoleSize) {
    ExecOptions opts;
    opts.tty = true;
    opts.console_size = testcontainers::TtySize{33, 123};

    const auto body = build_exec_create_body({"top"}, opts);
    ASSERT_TRUE(body.contains("ConsoleSize"));
    // Docker's wire order is [height, width] — rows FIRST; swapping them is
    // invisible on square-ish defaults, so pin an asymmetric pair.
    EXPECT_EQ(body["ConsoleSize"], nlohmann::json({33, 123}));

    EXPECT_FALSE(build_exec_create_body({"top"}, ExecOptions{}).contains("ConsoleSize"));
}

TEST(ApiMapping, ParseExecStatus) {
    // While the command runs, ExitCode is null (a moby pointer type): it must
    // read as absent — a value() lookup would throw on present-but-null.
    const auto running = parse_exec_status(R"({"Running": true, "ExitCode": null})");
    EXPECT_TRUE(running.running);
    EXPECT_FALSE(running.exit_code.has_value());

    const auto finished = parse_exec_status(R"({"Running": false, "ExitCode": 7})");
    EXPECT_FALSE(finished.running);
    ASSERT_TRUE(finished.exit_code.has_value());
    EXPECT_EQ(*finished.exit_code, 7);

    // Degenerate bodies stay parseable: everything defaults.
    const auto empty = parse_exec_status("{}");
    EXPECT_FALSE(empty.running);
    EXPECT_FALSE(empty.exit_code.has_value());
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
    EXPECT_FALSE(info.health_status.has_value()); // no Health section here
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

TEST(ApiMapping, ParseInspectHostConfigEcho) {
    const std::string body = R"({
        "Id": "abc123",
        "State": {"Status": "running", "Running": true},
        "HostConfig": {
            "Memory": 268435456,
            "ShmSize": 134217728,
            "NanoCpus": 500000000,
            "CpusetCpus": "0-2,7",
            "PidsLimit": 64,
            "RestartPolicy": {"Name": "on-failure", "MaximumRetryCount": 3},
            "Dns": ["192.0.2.53"],
            "DnsSearch": ["svc.test.internal"],
            "DnsOptions": ["ndots:2"],
            "Sysctls": {"net.ipv4.ip_unprivileged_port_start": "1000"},
            "Devices": [{"PathOnHost": "/dev/fuse",
                         "PathInContainer": "/dev/tc-fuse",
                         "CgroupPermissions": "rw"}]
        }
    })";

    const auto info = parse_inspect(body);
    const HostConfigInspect& hc = info.host_config;
    EXPECT_EQ(hc.memory_bytes, 268435456);
    EXPECT_EQ(hc.shm_size_bytes, 134217728);
    EXPECT_EQ(hc.nano_cpus, 500000000);
    EXPECT_EQ(hc.cpuset_cpus, "0-2,7");
    ASSERT_TRUE(hc.pids_limit.has_value());
    EXPECT_EQ(*hc.pids_limit, 64);
    EXPECT_EQ(hc.restart_policy.name, "on-failure");
    EXPECT_EQ(hc.restart_policy.maximum_retry_count, 3);
    EXPECT_EQ(hc.dns_servers, std::vector<std::string>{"192.0.2.53"});
    EXPECT_EQ(hc.dns_search, std::vector<std::string>{"svc.test.internal"});
    EXPECT_EQ(hc.dns_options, std::vector<std::string>{"ndots:2"});
    EXPECT_EQ(hc.sysctls.at("net.ipv4.ip_unprivileged_port_start"), "1000");
    ASSERT_EQ(hc.devices.size(), 1u);
    EXPECT_EQ(hc.devices[0].path_on_host, "/dev/fuse");
    EXPECT_EQ(hc.devices[0].path_in_container, "/dev/tc-fuse");
    EXPECT_EQ(hc.devices[0].cgroup_permissions, "rw");
}

TEST(ApiMapping, ParseInspectHostConfigAbsentAndNulls) {
    // No HostConfig at all: the zero state, nothing throws.
    const auto bare = parse_inspect(R"({"Id": "abc", "State": {"Running": true}})");
    EXPECT_EQ(bare.host_config.memory_bytes, 0);
    EXPECT_EQ(bare.host_config.nano_cpus, 0);
    EXPECT_EQ(bare.host_config.cpuset_cpus, "");
    EXPECT_FALSE(bare.host_config.pids_limit.has_value());
    EXPECT_EQ(bare.host_config.restart_policy.name, "");
    EXPECT_TRUE(bare.host_config.dns_servers.empty());
    EXPECT_TRUE(bare.host_config.sysctls.empty());
    EXPECT_TRUE(bare.host_config.devices.empty());

    // Null members (a daemon reports "no pids limit" as null; empty Sysctls /
    // Devices arrive as null too): still the zero state, never a type error.
    const auto nulls = parse_inspect(R"({
        "Id": "abc",
        "HostConfig": {"PidsLimit": null, "RestartPolicy": null, "Dns": null,
                       "Sysctls": null, "Devices": null}
    })");
    EXPECT_FALSE(nulls.host_config.pids_limit.has_value());
    EXPECT_EQ(nulls.host_config.restart_policy.name, "");
    EXPECT_EQ(nulls.host_config.restart_policy.maximum_retry_count, 0);
    EXPECT_TRUE(nulls.host_config.dns_servers.empty());
    EXPECT_TRUE(nulls.host_config.sysctls.empty());
    EXPECT_TRUE(nulls.host_config.devices.empty());
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
    EXPECT_EQ(split_image("alpine:"), (P{"alpine", "latest"})); // trailing ':' = no tag
    EXPECT_EQ(split_image("ghcr.io/owner/img:1.2"), (P{"ghcr.io/owner/img", "1.2"}));
    EXPECT_EQ(split_image("my-reg:5000/img"), (P{"my-reg:5000/img", "latest"}));

    // Digest references split at the '@'; the ':' INSIDE the digest is not a
    // tag separator, and a registry port before the last '/' still is not.
    EXPECT_EQ(split_image("redis@sha256:0123abcd"), (P{"redis", "sha256:0123abcd"}));
    EXPECT_EQ(split_image("my-reg:5000/ns/img@sha256:0123abcd"),
              (P{"my-reg:5000/ns/img", "sha256:0123abcd"}));
    EXPECT_EQ(split_image("img@"), (P{"img", "latest"})); // malformed: like a bare ':'
}

TEST(ApiMapping, JoinImage) {
    using testcontainers::docker::join_image;
    EXPECT_EQ(join_image("alpine", "3.20"), "alpine:3.20");
    EXPECT_EQ(join_image("my-reg:5000/img", "latest"), "my-reg:5000/img:latest");
    // A digest re-attaches with '@' — ':' would render an invalid reference.
    EXPECT_EQ(join_image("redis", "sha256:0123abcd"), "redis@sha256:0123abcd");

    // Round-trips: join(split(x)) reproduces the reference (modulo the
    // explicit ":latest" a bare name gains).
    const auto [name, tag] = split_image("my-reg:5000/ns/img@sha256:0123abcd");
    EXPECT_EQ(join_image(name, tag), "my-reg:5000/ns/img@sha256:0123abcd");
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
    const std::string stream = R"({"status":"Pulling from library/alpine"})"
                               "\n"
                               R"({"status":"Download complete"})"
                               "\n";
    EXPECT_NO_THROW(throw_if_pull_error(stream, "alpine:3.20"));
}

TEST(ApiMapping, PullNonStringErrorThrows) {
    // ANY "error" key is the daemon reporting a failed pull (docker's jsonmessage
    // errors on any non-nil error); a non-string payload is dumped, never
    // swallowed — and it must surface as DockerError, not raw json::type_error.
    const std::string stream = R"({"status":"Pulling from library/alpine"})"
                               "\n"
                               R"({"error":{"code":500}})"
                               "\n";
    EXPECT_THROW(throw_if_pull_error(stream, "alpine:3.20"), DockerError);
}

TEST(ApiMapping, BuildScannerEmitsStreamLines) {
    // Lines split across feed() chunks at arbitrary byte boundaries must still
    // come out whole and in order.
    const std::string stream = R"({"stream":"Step 1/2 : FROM alpine:3.20\n"})"
                               "\n"
                               R"({"stream":"step-output\n"})"
                               "\n"
                               R"({"stream":"Successfully built abc123\n"})"
                               "\n";

    std::vector<std::string> seen;
    BuildStreamScanner scanner("img:latest", [&](std::string_view s) { seen.emplace_back(s); });
    // Feed in awkward 7-byte chunks to exercise the carry-over buffer.
    for (std::size_t at = 0; at < stream.size(); at += 7) {
        scanner.feed(std::string_view(stream).substr(at, 7));
    }
    EXPECT_NO_THROW(scanner.finish());

    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], "Step 1/2 : FROM alpine:3.20\n");
    EXPECT_EQ(seen[1], "step-output\n");
    EXPECT_EQ(seen[2], "Successfully built abc123\n");
}

TEST(ApiMapping, BuildScannerErrorThrowsWithOutputTail) {
    // The daemon's error message AND the step output that preceded it must both
    // be in the exception — that output is what makes the failure debuggable.
    const std::string stream = R"({"stream":"Step 2/2 : RUN make\n"})"
                               "\n"
                               R"({"stream":"make: missing-dep not found\n"})"
                               "\n"
                               R"({"errorDetail":{"message":"boom"},"error":"boom"})"
                               "\n";

    BuildStreamScanner scanner("img:latest");
    scanner.feed(stream);
    try {
        scanner.finish();
        FAIL() << "finish() must throw on a recorded build error";
    } catch (const DockerError& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("boom"), std::string::npos) << what;
        EXPECT_NE(what.find("missing-dep not found"), std::string::npos) << what;
        EXPECT_EQ(e.resource_id(), "img:latest");
    }
}

TEST(ApiMapping, BuildScannerErrorDetailWithoutError) {
    // Some daemons emit only errorDetail; a non-string payload is dumped.
    BuildStreamScanner scanner("img:latest");
    scanner.feed(R"({"errorDetail":{"message":"detail-only"}})"
                 "\n");
    EXPECT_THROW(scanner.finish(), DockerError);
}

TEST(ApiMapping, BuildScannerTrailingLineWithoutNewline) {
    // A truncated stream whose final (error) line lacks the trailing '\n' must
    // still be scanned by finish() — otherwise the failure would be swallowed.
    BuildStreamScanner scanner("img:latest");
    scanner.feed(R"({"error":"cut off"})"); // no newline
    EXPECT_THROW(scanner.finish(), DockerError);
}

TEST(ApiMapping, BuildScannerTailIsBounded) {
    // Far more than the 4 KiB tail cap of step output, then a failure: the
    // exception must carry the LAST bytes (the output near the error), bounded,
    // with the early lines dropped.
    BuildStreamScanner scanner("img:latest");
    for (int i = 0; i < 200; ++i) {
        scanner.feed(R"({"stream":"line-)" + std::to_string(i) + std::string(60, 'x') + "\\n\"}\n");
    }
    scanner.feed(R"({"stream":"just-before-the-error\n"})"
                 "\n"
                 R"({"error":"boom"})"
                 "\n");
    try {
        scanner.finish();
        FAIL() << "finish() must throw on a recorded build error";
    } catch (const DockerError& e) {
        const std::string what = e.what();
        EXPECT_LT(what.size(), 4096u + 256u) << "tail not bounded; size=" << what.size();
        EXPECT_NE(what.find("just-before-the-error"), std::string::npos) << what;
        EXPECT_NE(what.find("line-199"), std::string::npos) << what; // recent output kept
        EXPECT_EQ(what.find("line-0x"), std::string::npos) << what;  // early output dropped
    }
}

TEST(ApiMapping, BuildScannerSuccessAndJunkLines) {
    // No consumer attached, a blank and a non-JSON line in the stream: nothing
    // throws on a successful build (best-effort parse, same as the pull scan).
    BuildStreamScanner scanner("img:latest");
    scanner.feed(R"({"stream":"Step 1/1 : FROM alpine:3.20\n"})"
                 "\n"
                 "\n"
                 "this is not json\n"
                 R"({"stream":"Successfully built abc123\n"})"
                 "\n");
    EXPECT_NO_THROW(scanner.finish());
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

TEST(ApiMapping, BuildQueryLabels) {
    BuildOptions options;
    options.tag = "myimg:latest";
    const auto identity = [](const std::string& v) { return v; };

    // No labels configured: the key must be absent entirely (an empty labels={}
    // would still be accepted by the daemon, but there is no reason to send it).
    EXPECT_EQ(build_build_query(options, identity).find("labels="), std::string::npos);

    options.labels = {{"org.testcontainers.session-id", "abc123"}, {"custom", "x"}};
    const std::string q = build_build_query(options, identity);
    const std::size_t pos = q.find("labels=");
    ASSERT_NE(pos, std::string::npos);
    std::string value = q.substr(pos + std::string("labels=").size());
    if (const std::size_t amp = value.find('&'); amp != std::string::npos) {
        value = value.substr(0, amp);
    }
    const auto parsed = nlohmann::json::parse(value);
    EXPECT_EQ(parsed["org.testcontainers.session-id"], "abc123");
    EXPECT_EQ(parsed["custom"], "x");
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
