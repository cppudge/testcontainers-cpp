#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/modules/ScyllaDB.hpp"

// Tests in this file (unit; no Docker daemon — the module's rendering rules
// via to_generic()):
//   ScyllaDBModuleConfig.DefaultRendersPinPortFlagsAndReadinessPair - the default config renders the pinned image, exposes 9042/tcp, owns the managed developer-mode/overprovisioned/smp/memory flags, installs the ordered log -> cqlsh readiness pair, and raises the startup budget to 120s.
//   ScyllaDBModuleConfig.TuningSettersAndUserArgsAppendLast - with_smp/with_memory rewrite the managed flags, with_datacenter appends --dc, and with_command_args land after everything so a user duplicate wins in the entrypoint's last-occurrence parsing.
//   ScyllaDBModuleConfig.InvalidTuningThrowsAtRender - smp < 1, an empty memory amount, and an empty with_datacenter name throw at render, before any daemon contact.
//   ScyllaDBModuleConfig.InitScriptsStageOrderedAndValidated - init scripts stage under /tmp with the zero-padded registration prefix, queue one started hook, the host-file overload stages under the file's bare name, and only *.cql names are accepted (bare names for the in-memory form).
//   ScyllaDBModuleConfig.NoInitScriptsMeansNoHook - without with_init_script the render owns no started hook.
//   ScyllaDBModuleConfig.CustomWaitReplacesDefaultPair - the first with_wait drops the module's readiness pair; further calls accumulate in order.
//   ScyllaDBModuleConfig.CustomizerRunsLastAndWins - a customizer sees the module-rendered builder and what it sets overrides the module's rendering.
//   ScyllaDBModuleConfig.WithImageRewritesReference - with_image swaps name and tag (bare name defaults to "latest") while the rest of the config survives.
//   ScyllaDBModuleConfig.RenderingIsIdempotent - to_generic() renders into a copy: repeated renders are equal and the config never accumulates cmd, copies, waits, or hooks.
//   ScyllaDBModuleConfig.PassThroughsLandOnTheImage - the env/label/network/alias/reuse/timeout/attempts pass-throughs reach the rendered builder.

using namespace testcontainers;
using modules::ScyllaDBImage;

TEST(ScyllaDBModuleConfig, DefaultRendersPinPortFlagsAndReadinessPair) {
    const GenericImage generic = ScyllaDBImage().to_generic();

    EXPECT_EQ(generic.image(), "scylladb/scylla");
    EXPECT_EQ(generic.tag(), "2026.1");
    ASSERT_EQ(generic.exposed_ports().size(), 1u);
    EXPECT_EQ(generic.exposed_ports()[0], tcp(9042));

    // The managed CI-shape flags; the entrypoint parses them, not env.
    EXPECT_EQ(generic.cmd(), (std::vector<std::string>{"--developer-mode=1", "--overprovisioned=1",
                                                       "--smp", "1", "--memory", "512M"}));
    EXPECT_TRUE(generic.env().empty());

    // Ordered pair: the log line gates out the long node init, the cqlsh
    // probe is the authoritative query-path proof.
    ASSERT_EQ(generic.waits().size(), 2u);
    const auto* log = std::get_if<wait_for::LogMessage>(&generic.waits()[0]);
    ASSERT_NE(log, nullptr);
    EXPECT_EQ(log->text, "Starting listening for CQL clients");
    const auto* probe = std::get_if<wait_for::Command>(&generic.waits()[1]);
    ASSERT_NE(probe, nullptr);
    ASSERT_EQ(probe->cmd.size(), 3u);
    EXPECT_EQ(probe->cmd[2],
              "cqlsh \"$(hostname)\" -e \"SELECT release_version FROM system.local\"");

    // A first boot initializes the data directory: the module raises the
    // budget over the core default.
    EXPECT_EQ(generic.startup_timeout(), std::chrono::seconds(120));
    EXPECT_TRUE(generic.started_hooks().empty());
}

TEST(ScyllaDBModuleConfig, TuningSettersAndUserArgsAppendLast) {
    ScyllaDBImage cfg;
    cfg.with_smp(2)
        .with_memory("1G")
        .with_datacenter("tcdc")
        .with_command_args({"--rack", "r1"})
        .with_command_arg("--smp=4"); // user duplicate: last occurrence wins server-side

    EXPECT_EQ(cfg.smp(), 2);
    EXPECT_EQ(cfg.memory(), "1G");
    EXPECT_EQ(cfg.datacenter(), "tcdc");

    const GenericImage generic = cfg.to_generic();
    EXPECT_EQ(generic.cmd(), (std::vector<std::string>{"--developer-mode=1", "--overprovisioned=1",
                                                       "--smp", "2", "--memory", "1G", "--dc",
                                                       "tcdc", "--rack", "r1", "--smp=4"}));
}

TEST(ScyllaDBModuleConfig, InvalidTuningThrowsAtRender) {
    EXPECT_THROW(ScyllaDBImage().with_smp(0).to_generic(), Error);
    EXPECT_THROW(ScyllaDBImage().with_memory("").to_generic(), Error);
    EXPECT_THROW(ScyllaDBImage().with_datacenter("").to_generic(), Error);
    // The unset default renders no --dc and is valid.
    EXPECT_NO_THROW(ScyllaDBImage().to_generic());
}

TEST(ScyllaDBModuleConfig, InitScriptsStageOrderedAndValidated) {
    ScyllaDBImage cfg;
    cfg.with_init_script("schema.cql", "CREATE KEYSPACE tc WITH replication = "
                                       "{'class': 'NetworkTopologyStrategy', "
                                       "'replication_factor': 1}")
        .with_init_script("seed.cql", "INSERT INTO tc.kv (k, v) VALUES ('a', 1)");

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.copy_to_sources().size(), 2u);
    EXPECT_EQ(generic.copy_to_sources()[0].target(), "/tmp/testcontainers-0000-schema.cql");
    EXPECT_EQ(generic.copy_to_sources()[1].target(), "/tmp/testcontainers-0001-seed.cql");
    ASSERT_EQ(generic.started_hooks().size(), 1u);

    // The host-file overload stages under the file's bare name (rendering
    // does not read the file, so no fixture is needed here).
    ScyllaDBImage from_file;
    from_file.with_init_script(std::filesystem::path("dir/schema.cql"));
    EXPECT_EQ(from_file.to_generic().copy_to_sources()[0].target(),
              "/tmp/testcontainers-0000-schema.cql");

    // Only .cql runs through cqlsh -f; anything else throws up front.
    EXPECT_THROW(ScyllaDBImage().with_init_script("a.sql", "-"), Error);
    EXPECT_THROW(ScyllaDBImage().with_init_script(".cql", "-"), Error);
    EXPECT_THROW(ScyllaDBImage().with_init_script("dir/a.cql", "-"), Error);
}

TEST(ScyllaDBModuleConfig, NoInitScriptsMeansNoHook) {
    const GenericImage generic = ScyllaDBImage().to_generic();
    EXPECT_TRUE(generic.started_hooks().empty());
    EXPECT_TRUE(generic.copy_to_sources().empty());
}

TEST(ScyllaDBModuleConfig, CustomWaitReplacesDefaultPair) {
    ScyllaDBImage cfg;
    cfg.with_wait(wait_for::log("init - serving"));
    cfg.with_wait(wait_for::listening_port(tcp(9042)));

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.waits().size(), 2u); // the default readiness pair is gone
    const auto* log = std::get_if<wait_for::LogMessage>(&generic.waits()[0]);
    ASSERT_NE(log, nullptr);
    EXPECT_EQ(log->text, "init - serving");
    EXPECT_NE(std::get_if<wait_for::Port>(&generic.waits()[1]), nullptr);
}

TEST(ScyllaDBModuleConfig, CustomizerRunsLastAndWins) {
    ScyllaDBImage cfg;
    cfg.with_customizer([](GenericImage& generic) {
           // Runs after the module's rendering, so the managed flags are
           // already applied.
           ASSERT_EQ(generic.cmd().size(), 6u);
           generic.with_label("team", "storage");
       })
        .with_customizer([](GenericImage& generic) { generic.with_env("TZ", "UTC"); });

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.env().size(), 1u); // the module itself sets no env
    EXPECT_EQ(generic.env()[0].first, "TZ");
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
}

TEST(ScyllaDBModuleConfig, WithImageRewritesReference) {
    ScyllaDBImage cfg;
    cfg.with_smp(2).with_image("scylladb/scylla:6.2");

    const GenericImage generic = cfg.to_generic();
    EXPECT_EQ(generic.image(), "scylladb/scylla");
    EXPECT_EQ(generic.tag(), "6.2");
    // The rest of the config survives the swap.
    EXPECT_EQ(generic.cmd()[3], "2");

    cfg.with_image("scylladb/scylla"); // bare name defaults to "latest"
    EXPECT_EQ(cfg.to_generic().image(), "scylladb/scylla");
    EXPECT_EQ(cfg.to_generic().tag(), "latest");
}

TEST(ScyllaDBModuleConfig, RenderingIsIdempotent) {
    ScyllaDBImage cfg;
    cfg.with_command_arg("--rack").with_command_arg("r1").with_init_script("seed.cql", "-- noop");

    const GenericImage first = cfg.to_generic();
    const GenericImage second = cfg.to_generic();
    EXPECT_EQ(first.cmd(), second.cmd());
    ASSERT_EQ(second.cmd().size(), 8u);             // managed flags + the two args, once
    ASSERT_EQ(second.copy_to_sources().size(), 1u); // staged once, never re-appended
    ASSERT_EQ(second.waits().size(), 2u);           // the readiness pair is rendered once
    ASSERT_EQ(second.started_hooks().size(), 1u);   // the hook is queued once
}

TEST(ScyllaDBModuleConfig, PassThroughsLandOnTheImage) {
    ScyllaDBImage cfg;
    cfg.with_env("TZ", "UTC")
        .with_label("team", "storage")
        .with_network("net-a")
        .with_network_alias("cql")
        .with_reuse()
        .with_startup_timeout(std::chrono::seconds(90))
        .with_startup_attempts(2);

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.env().size(), 1u); // TZ only — the module sets no env
    EXPECT_EQ(generic.env()[0].first, "TZ");
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
    EXPECT_EQ(generic.network().value_or(""), "net-a");
    ASSERT_EQ(generic.network_aliases().size(), 1u);
    EXPECT_EQ(generic.network_aliases()[0], "cql");
    EXPECT_TRUE(generic.reuse());
    EXPECT_EQ(generic.startup_timeout(), std::chrono::seconds(90)); // user override wins
    EXPECT_EQ(generic.startup_attempts(), 2);
}
