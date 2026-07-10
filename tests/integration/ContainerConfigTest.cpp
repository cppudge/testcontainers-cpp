#include <gtest/gtest.h>

#include <cstdint>
#include <exception>
#include <optional>
#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/Logs.hpp"

#include "CapMask.hpp"
#include "EngineGuard.hpp"
#include "TempPaths.hpp"

// Tests in this file (integration; require a Docker daemon):
//   ContainerConfig.WorkingDirAndUser - working dir and user are applied so the process runs in /tmp as uid 1000.
//   ContainerConfig.EntrypointOverride - an explicit entrypoint overrides the image default so `echo` prints the cmd arg.
//   ContainerConfig.TmpfsMount - a tmpfs mount appears as a tmpfs filesystem at its target in /proc/mounts.
//   ContainerConfig.UlimitApplied - a ulimit is applied so the soft nofile limit reported inside the container matches.
//   ContainerConfig.ExtraHostApplied - an extra host (via the typed setter) resolves to its mapped IP inside the container.
//   ContainerConfig.CustomSubstitutorRewritesImage - a custom image-name substitutor rewrites a bogus reference to a runnable one used at create.
//   ContainerConfig.AlwaysPullPolicyStarts - ImagePullPolicy::Always pulls before create and the container still starts and runs.
//   ContainerConfig.BindMountReadOnly - a read-only bind mount exposes a host file's content inside the container and rejects writes.
//   ContainerConfig.MemoryAndShmLimitsVisibleInside - with_memory_limit / with_shm_size land in the container's cgroup limit and /dev/shm size.
//   ContainerConfig.CapAddDropReflectedInBounding - with_cap_add("SYS_TIME") sets and with_cap_drop("CHOWN") clears the matching bit in the container's bounding capability set.
//   ContainerConfig.CpuPidsCpusetLimitsVisibleInside - with_cpu_limit / with_pids_limit / with_cpuset_cpus land in the container's cgroup cpu, pids, and cpuset files.
//   ContainerConfig.SysctlAppliedInside - a with_sysctl value reads back from /proc/sys inside the container, distinct from Docker's own default.
//   ContainerConfig.DnsConfigWrittenToResolvConf - with_dns_server / with_dns_search / with_dns_option are written into the container's /etc/resolv.conf on the default bridge.
//   ContainerConfig.DeviceMappedInside - with_device maps a daemon-host device node to a (renamed) path inside the container.

using namespace testcontainers;

// Requires a reachable Docker daemon; skipped if none is available. The
// short-lived containers below use wait_for::exit(), so start() returns only
// after the process finishes; logs() then reads its captured output. The
// Container still auto-removes on scope exit.
class ContainerConfig : public ::testing::Test {
protected:
    void SetUp() override {
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

TEST_F(ContainerConfig, WorkingDirAndUser) {
    Container c = GenericImage("alpine", "3.20")
                      .with_working_dir("/tmp")
                      .with_user("1000:1000")
                      .with_cmd({"sh", "-c", "pwd; id -u"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("/tmp"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_NE(logs.stdout_data.find("1000"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, EntrypointOverride) {
    Container c = GenericImage("alpine", "3.20")
                      .with_entrypoint({"echo"})
                      .with_cmd({"hello-entry"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("hello-entry"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, TmpfsMount) {
    Container c = GenericImage("alpine", "3.20")
                      .with_mount(Mount::tmpfs("/cache"))
                      .with_cmd({"sh", "-c", "grep /cache /proc/mounts"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("/cache"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_NE(logs.stdout_data.find("tmpfs"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, UlimitApplied) {
    Container c = GenericImage("alpine", "3.20")
                      .with_cmd({"sh", "-c", "ulimit -n"})
                      .with_ulimit("nofile", 4096, 8192)
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("4096"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, ExtraHostApplied) {
    Container c = GenericImage("alpine", "3.20")
                      .with_extra_host("myhost", "1.2.3.4")
                      .with_cmd({"sh", "-c", "getent hosts myhost || cat /etc/hosts"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("1.2.3.4"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_NE(logs.stdout_data.find("myhost"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, CustomSubstitutorRewritesImage) {
    // The original reference is bogus; the substitutor must be what reaches create
    // (otherwise the bogus image would fail to pull). Proves the rewrite flows in.
    Container c = GenericImage("nonexistent-image-name", "v0")
                      .with_image_name_substitutor(
                          [](const std::string&) { return std::string("alpine:3.20"); })
                      .with_cmd({"sh", "-c", "echo substituted"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("substituted"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, AlwaysPullPolicyStarts) {
    // Exercises the always-pull-before-create path; alpine:3.20 is public so the
    // pull succeeds and the container still runs.
    Container c = GenericImage("alpine", "3.20")
                      .with_image_pull_policy(ImagePullPolicy::Always)
                      .with_cmd({"sh", "-c", "echo ok"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("ok"), std::string::npos) << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, BindMountReadOnly) {
    const tcit::TempDirWithFile host("f.txt", "bind-content");

    // Read the bind-mounted file, then prove the read-only flag: the write must
    // fail. (Both outcomes are printed so a failure shows what happened.)
    Container c = GenericImage("alpine", "3.20")
                      .with_mount(Mount::bind(host.mount_source(), "/hostdata").with_read_only())
                      .with_cmd({"sh", "-c",
                                 "cat /hostdata/f.txt; "
                                 "touch /hostdata/x 2>/dev/null && echo write-allowed"
                                 " || echo write-denied"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("bind-content"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_NE(logs.stdout_data.find("write-denied"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_EQ(logs.stdout_data.find("write-allowed"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, MemoryAndShmLimitsVisibleInside) {
    // The daemon's inspect does not echo Memory/ShmSize back, so assert from
    // INSIDE: the cgroup memory limit (v2 path, v1 fallback) and /dev/shm size.
    Container c = GenericImage("alpine", "3.20")
                      .with_memory_limit(256LL * 1024 * 1024)
                      .with_shm_size(128LL * 1024 * 1024)
                      .with_cmd({"sh", "-c",
                                 "cat /sys/fs/cgroup/memory.max 2>/dev/null"
                                 " || cat /sys/fs/cgroup/memory/memory.limit_in_bytes; "
                                 "df -k /dev/shm"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("268435456"), std::string::npos)
        << "cgroup memory limit missing; stdout was: " << logs.stdout_data;
    // df -k reports /dev/shm in 1K blocks: 128 MiB = 131072.
    EXPECT_NE(logs.stdout_data.find("131072"), std::string::npos)
        << "/dev/shm size missing; stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, CpuPidsCpusetLimitsVisibleInside) {
    // The daemon silently ignores unknown create-body fields, so a started
    // container proves nothing about field names — read each applied limit
    // back from the container's own cgroup files (v2 path, v1 fallback; same
    // pattern as the memory test). The values are bracketed so a wider
    // default (e.g. cpuset "0-7") cannot false-match. 0.5 CPUs = quota 50000
    // of period 100000, printed as "50000 100000" (v2) or "50000" (v1).
    Container c = GenericImage("alpine", "3.20")
                      .with_cpu_limit(0.5)
                      .with_pids_limit(64)
                      .with_cpuset_cpus("0")
                      .with_cmd({"sh", "-c",
                                 "echo \"cpu=[$(cat /sys/fs/cgroup/cpu.max 2>/dev/null"
                                 " || cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us)]\"; "
                                 "echo \"pids=[$(cat /sys/fs/cgroup/pids.max 2>/dev/null"
                                 " || cat /sys/fs/cgroup/pids/pids.max)]\"; "
                                 "echo \"cpuset=[$(cat /sys/fs/cgroup/cpuset.cpus.effective"
                                 " 2>/dev/null || cat /sys/fs/cgroup/cpuset/cpuset.cpus)]\""})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("cpu=[50000"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_NE(logs.stdout_data.find("pids=[64]"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_NE(logs.stdout_data.find("cpuset=[0]"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, SysctlAppliedInside) {
    // 1000 differs from BOTH defaults a container could otherwise show — the
    // kernel's 1024 and the 0 modern daemons set themselves (moby 20.10+) —
    // so reading it back proves OUR value flowed through on any daemon.
    Container c = GenericImage("alpine", "3.20")
                      .with_sysctl("net.ipv4.ip_unprivileged_port_start", "1000")
                      .with_cmd({"sh", "-c",
                                 "echo \"port-start=[$("
                                 "cat /proc/sys/net/ipv4/ip_unprivileged_port_start)]\""})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("port-start=[1000]"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, DnsConfigWrittenToResolvConf) {
    // On the default bridge network the daemon writes Dns/DnsSearch/DnsOptions
    // verbatim into the container's /etc/resolv.conf (a user-defined network
    // would interpose its embedded 127.0.0.11 resolver instead). Nothing needs
    // to answer at the server address — 192.0.2.53 is TEST-NET-1, never routed.
    Container c = GenericImage("alpine", "3.20")
                      .with_dns_server("192.0.2.53")
                      .with_dns_search("svc.test.internal")
                      .with_dns_option("ndots:2")
                      .with_cmd({"sh", "-c", "cat /etc/resolv.conf"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    EXPECT_NE(logs.stdout_data.find("nameserver 192.0.2.53"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_NE(logs.stdout_data.find("svc.test.internal"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_NE(logs.stdout_data.find("ndots:2"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, DeviceMappedInside) {
    // /dev/fuse exists on any modern daemon host (Docker Desktop VM, CI
    // runners) but is NOT in a container's default device set, so its
    // presence inside proves the mapping applied; mapping it under a NEW name
    // proves PathInContainer is honored (not just a default-set leak).
    std::optional<Container> c;
    try {
        c.emplace(GenericImage("alpine", "3.20")
                      .with_device("/dev/fuse", "/dev/tc-fuse")
                      .with_cmd({"sh", "-c",
                                 "test -c /dev/tc-fuse && echo device-present"
                                 " || echo device-missing"})
                      .with_wait(wait_for::exit())
                      .start());
    } catch (const DockerError& e) {
        // A daemon HOST without the node errors at create ("error gathering
        // device information") — an environment gap, not a mapping bug: a
        // wrong Devices field name would be silently IGNORED and surface as
        // device-missing in the assert below. Skip on such hosts.
        if (std::string(e.what()).find("/dev/fuse") != std::string::npos) {
            GTEST_SKIP() << "daemon host lacks /dev/fuse: " << e.what();
        }
        throw;
    }

    const ContainerLogs logs = c->logs();
    EXPECT_NE(logs.stdout_data.find("device-present"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
    EXPECT_EQ(logs.stdout_data.find("device-missing"), std::string::npos)
        << "stdout was: " << logs.stdout_data;
}

TEST_F(ContainerConfig, CapAddDropReflectedInBounding) {
    // Docker's default capability set includes CHOWN (bit 0) and excludes
    // SYS_TIME (bit 25), so each assertion below proves its own flag flowed
    // through — independent of the daemon's exact default mask.
    Container c = GenericImage("alpine", "3.20")
                      .with_cap_add("SYS_TIME")
                      .with_cap_drop("CHOWN")
                      .with_cmd({"sh", "-c", "grep CapBnd /proc/self/status"})
                      .with_wait(wait_for::exit())
                      .start();

    const ContainerLogs logs = c.logs();
    const std::uint64_t bounding = tcit::cap_mask_after(logs.stdout_data, "CapBnd:");
    ASSERT_NE(bounding, 0u) << "no CapBnd line; stdout was: " << logs.stdout_data;
    EXPECT_TRUE(bounding & (std::uint64_t{1} << 25))
        << "CAP_SYS_TIME not granted; CapBnd was: " << logs.stdout_data;
    EXPECT_FALSE(bounding & std::uint64_t{1})
        << "CAP_CHOWN not dropped; CapBnd was: " << logs.stdout_data;
}
