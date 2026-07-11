#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "testcontainers/GenericImage.hpp"

// Tests in this file:
//   GenericImage.DefaultsTagAndTimeout - a freshly constructed image defaults to the "latest" tag and a 60s startup timeout.
//   GenericImage.ExplicitTag - the two-argument constructor stores the given tag.
//   GenericImage.GettersReflectBuilders - each with_* builder records into the matching getter.
//   GenericImage.ConfigBuildersReflectGetters - entrypoint/working-dir/user/privileged/isolation/mount builders record into the matching getters.
//   GenericImage.ConfigDefaults - a freshly constructed image has no entrypoint/working-dir/user/isolation/mounts and is not privileged.
//   GenericImage.NetworkDefaults - a freshly constructed image has no network, static IPv4, or container name set.
//   GenericImage.NetworkBuildersReflectGetters - with_network, with_static_ipv4, and with_container_name record into the matching getters.
//   GenericImage.PlatformDefaultsAndBuilder - platform is unset by default and with_platform records into the getter.
//   GenericImage.ConfigChainsOnRvalue - the new config builders chain on a temporary rvalue.
//   GenericImage.ChainsOnLvalue - with_* chains on a named lvalue and accumulates all settings.
//   GenericImage.ChainsOnRvalue - with_* chains on a temporary rvalue and accumulates all settings.
//   GenericImage.ReusableAfterWith - a named image survives a with_* call and reflects both early and later settings (no use-after-move).
//   GenericImage.FromReference - from_reference splits "name[:tag]" into image and tag, defaulting to "latest" and handling a registry host:port.
//   GenericImage.WithImageReplacesReferenceKeepingOptions - with_image swaps name and tag (same parsing as from_reference) while every other configured option survives.
//   GenericImage.LifecycleHookDefaults - a freshly constructed image has no created/starting/started/stopping hooks and a single startup attempt.
//   GenericImage.LifecycleHooksGrowVectors - each with_*_hook builder appends to the matching hook vector (in order across repeated calls).
//   GenericImage.StartupAttemptsBuilder - with_startup_attempts records the count and clamps values < 1 to 1.
//   GenericImage.LifecycleBuildersChainOnRvalue - the hook/attempts builders chain on a temporary rvalue.
//   GenericImage.ToRequestSnapshotsBuilderState - to_request() carries the translated create spec (image ref, "K=V" env, "port/proto" strings, publish-all) plus every orchestration field, one-to-one with the builders.
//   GenericImage.ExposedHostPortsDefaultEmpty - a freshly constructed image exposes no host ports.
//   GenericImage.ExposedHostPortsAccumulateAndSnapshot - with_exposed_host_port accumulates in call order, chains on an rvalue, and to_request() carries the ports without touching the create spec (no ExtraHosts entry until run()).
//   GenericImage.PullPolicyOverloadsReplaceEachOther - the age overload sets Default + pull_max_age (carried into to_request), the enum overload clears a previously-set budget, and a fresh image has no budget.

using namespace testcontainers;

TEST(GenericImage, DefaultsTagAndTimeout) {
    const GenericImage img("redis");
    EXPECT_EQ(img.image(), "redis");
    EXPECT_EQ(img.tag(), "latest");
    EXPECT_EQ(img.startup_timeout(), std::chrono::seconds(60));
    EXPECT_TRUE(img.exposed_ports().empty());
    EXPECT_TRUE(img.env().empty());
    EXPECT_TRUE(img.cmd().empty());
    EXPECT_TRUE(img.labels().empty());
    EXPECT_TRUE(img.waits().empty());
}

TEST(GenericImage, ExplicitTag) {
    const GenericImage img("redis", "7.2");
    EXPECT_EQ(img.image(), "redis");
    EXPECT_EQ(img.tag(), "7.2");
}

TEST(GenericImage, GettersReflectBuilders) {
    GenericImage img("redis", "7.2");
    img.with_exposed_port(tcp(6379))
        .with_env("MODE", "standalone")
        .with_cmd({"redis-server", "--appendonly", "yes"})
        .with_label("owner", "tc")
        .with_wait(wait_for::stdout_message("Ready"))
        .with_startup_timeout(std::chrono::milliseconds(5000));

    ASSERT_EQ(img.exposed_ports().size(), 1u);
    EXPECT_EQ(img.exposed_ports()[0], tcp(6379));

    ASSERT_EQ(img.env().size(), 1u);
    EXPECT_EQ(img.env()[0].first, "MODE");
    EXPECT_EQ(img.env()[0].second, "standalone");

    EXPECT_EQ(img.cmd(), (std::vector<std::string>{"redis-server", "--appendonly", "yes"}));

    ASSERT_EQ(img.labels().size(), 1u);
    EXPECT_EQ(img.labels()[0].first, "owner");
    EXPECT_EQ(img.labels()[0].second, "tc");

    ASSERT_EQ(img.waits().size(), 1u);
    ASSERT_TRUE(std::holds_alternative<wait_for::LogMessage>(img.waits()[0]));
    EXPECT_EQ(std::get<wait_for::LogMessage>(img.waits()[0]).text, "Ready");

    EXPECT_EQ(img.startup_timeout(), std::chrono::milliseconds(5000));
}

TEST(GenericImage, ConfigDefaults) {
    const GenericImage img("alpine", "3.20");
    EXPECT_TRUE(img.entrypoint().empty());
    EXPECT_FALSE(img.working_dir().has_value());
    EXPECT_FALSE(img.user().has_value());
    EXPECT_FALSE(img.privileged());
    EXPECT_FALSE(img.isolation().has_value());
    EXPECT_TRUE(img.mounts().empty());
}

TEST(GenericImage, NetworkDefaults) {
    const GenericImage img("alpine", "3.20");
    EXPECT_FALSE(img.network().has_value());
    EXPECT_FALSE(img.static_ipv4().has_value());
    EXPECT_FALSE(img.container_name().has_value());
}

TEST(GenericImage, NetworkBuildersReflectGetters) {
    GenericImage img("redis", "7.2");
    img.with_network("my-net").with_static_ipv4("10.246.200.11").with_container_name("redis-srv");

    ASSERT_TRUE(img.network().has_value());
    EXPECT_EQ(*img.network(), "my-net");
    ASSERT_TRUE(img.static_ipv4().has_value());
    EXPECT_EQ(*img.static_ipv4(), "10.246.200.11");
    ASSERT_TRUE(img.container_name().has_value());
    EXPECT_EQ(*img.container_name(), "redis-srv");
}

TEST(GenericImage, PlatformDefaultsAndBuilder) {
    const GenericImage def("mcr.microsoft.com/windows/nanoserver", "ltsc2025");
    EXPECT_FALSE(def.platform().has_value());

    GenericImage img("mcr.microsoft.com/windows/nanoserver", "ltsc2025");
    img.with_platform("windows/amd64");
    ASSERT_TRUE(img.platform().has_value());
    EXPECT_EQ(*img.platform(), "windows/amd64");

    // Chains on a temporary rvalue too.
    const GenericImage chained = GenericImage("alpine", "3.20").with_platform("linux/arm64");
    ASSERT_TRUE(chained.platform().has_value());
    EXPECT_EQ(*chained.platform(), "linux/arm64");
}

TEST(GenericImage, ConfigBuildersReflectGetters) {
    GenericImage img("alpine", "3.20");
    img.with_entrypoint({"echo"})
        .with_working_dir("/tmp")
        .with_user("1000:1000")
        .with_privileged()
        .with_isolation("process")
        .with_mount(Mount::tmpfs("/cache").with_tmpfs_size(1024))
        .with_mount(Mount::bind("/host", "/data").with_read_only());

    EXPECT_EQ(img.entrypoint(), (std::vector<std::string>{"echo"}));
    ASSERT_TRUE(img.working_dir().has_value());
    EXPECT_EQ(*img.working_dir(), "/tmp");
    ASSERT_TRUE(img.user().has_value());
    EXPECT_EQ(*img.user(), "1000:1000");
    EXPECT_TRUE(img.privileged());
    ASSERT_TRUE(img.isolation().has_value());
    EXPECT_EQ(*img.isolation(), "process");

    ASSERT_EQ(img.mounts().size(), 2u);
    EXPECT_EQ(img.mounts()[0].type(), MountType::Tmpfs);
    EXPECT_EQ(img.mounts()[0].target(), "/cache");
    ASSERT_TRUE(img.mounts()[0].tmpfs_size().has_value());
    EXPECT_EQ(*img.mounts()[0].tmpfs_size(), 1024);
    EXPECT_EQ(img.mounts()[1].type(), MountType::Bind);
    EXPECT_TRUE(img.mounts()[1].is_read_only());
}

TEST(GenericImage, ConfigChainsOnRvalue) {
    const GenericImage img = GenericImage("alpine", "3.20")
                                 .with_entrypoint({"echo"})
                                 .with_working_dir("/tmp")
                                 .with_user("root")
                                 .with_privileged(true)
                                 .with_mount(Mount::tmpfs("/cache"));
    EXPECT_EQ(img.entrypoint(), (std::vector<std::string>{"echo"}));
    EXPECT_EQ(*img.working_dir(), "/tmp");
    EXPECT_EQ(*img.user(), "root");
    EXPECT_TRUE(img.privileged());
    ASSERT_EQ(img.mounts().size(), 1u);
    EXPECT_EQ(img.mounts()[0].type(), MountType::Tmpfs);
}

TEST(GenericImage, ChainsOnLvalue) {
    GenericImage img("redis", "7.2");
    GenericImage& ref = img.with_exposed_port(tcp(6379)).with_env("A", "1");
    EXPECT_EQ(&ref, &img); // chaining returns the same object
    EXPECT_EQ(img.exposed_ports().size(), 1u);
    EXPECT_EQ(img.env().size(), 1u);
}

TEST(GenericImage, ChainsOnRvalue) {
    const GenericImage img = GenericImage("redis", "7.2")
                                 .with_exposed_port(tcp(6379))
                                 .with_env("A", "1")
                                 .with_wait(wait_for::log("Ready"));
    EXPECT_EQ(img.exposed_ports().size(), 1u);
    EXPECT_EQ(img.env().size(), 1u);
    EXPECT_EQ(img.waits().size(), 1u);
}

TEST(GenericImage, ReusableAfterWith) {
    // The key anti-use-after-move check: a named image stays valid across
    // separate with_* calls, and getters reflect every change.
    GenericImage img("redis", "7.2");
    img.with_exposed_port(tcp(6379)); // configure once
    EXPECT_EQ(img.exposed_ports().size(), 1u);

    img.with_env("MODE", "standalone"); // configure more
    EXPECT_EQ(img.env().size(), 1u);

    // Earlier setting is still present alongside the later one.
    ASSERT_EQ(img.exposed_ports().size(), 1u);
    EXPECT_EQ(img.exposed_ports()[0], tcp(6379));
    ASSERT_EQ(img.env().size(), 1u);
    EXPECT_EQ(img.env()[0].first, "MODE");

    img.with_exposed_port(tcp(6380)); // and again
    EXPECT_EQ(img.exposed_ports().size(), 2u);
}

TEST(GenericImage, FromReference) {
    const GenericImage with_tag = GenericImage::from_reference("repo:tag");
    EXPECT_EQ(with_tag.image(), "repo");
    EXPECT_EQ(with_tag.tag(), "tag");

    const GenericImage no_tag = GenericImage::from_reference("repo");
    EXPECT_EQ(no_tag.image(), "repo");
    EXPECT_EQ(no_tag.tag(), "latest"); // tag defaults to latest

    // A registry host:port is not mistaken for a tag.
    const GenericImage with_registry = GenericImage::from_reference("host:5000/repo:1.2");
    EXPECT_EQ(with_registry.image(), "host:5000/repo");
    EXPECT_EQ(with_registry.tag(), "1.2");
}

TEST(GenericImage, WithImageReplacesReferenceKeepingOptions) {
    GenericImage img("redis", "7.2");
    img.with_exposed_port(tcp(6379)).with_env("MODE", "standalone");

    img.with_image("valkey/valkey:8.0");
    EXPECT_EQ(img.image(), "valkey/valkey");
    EXPECT_EQ(img.tag(), "8.0");
    // Every other configured option survives the swap.
    ASSERT_EQ(img.exposed_ports().size(), 1u);
    EXPECT_EQ(img.exposed_ports()[0], tcp(6379));
    ASSERT_EQ(img.env().size(), 1u);
    EXPECT_EQ(img.env()[0].first, "MODE");

    // Same parsing as from_reference: bare name defaults to "latest", a
    // registry host:port is not mistaken for a tag.
    EXPECT_EQ(img.with_image("repo").tag(), "latest");
    img.with_image("host:5000/repo:1.2");
    EXPECT_EQ(img.image(), "host:5000/repo");
    EXPECT_EQ(img.tag(), "1.2");
}

TEST(GenericImage, LifecycleHookDefaults) {
    const GenericImage img("alpine", "3.20");
    EXPECT_TRUE(img.created_hooks().empty());
    EXPECT_TRUE(img.starting_hooks().empty());
    EXPECT_TRUE(img.started_hooks().empty());
    EXPECT_TRUE(img.stopping_hooks().empty());
    EXPECT_EQ(img.startup_attempts(), 1); // a single attempt, no retry, by default
}

TEST(GenericImage, LifecycleHooksGrowVectors) {
    const LifecycleHook noop = [](DockerClient&, const std::string&) {};

    GenericImage img("alpine", "3.20");
    img.with_created_hook(noop)
        .with_created_hook(noop) // appends, does not replace
        .with_starting_hook(noop)
        .with_started_hook(noop)
        .with_stopping_hook(noop);

    EXPECT_EQ(img.created_hooks().size(), 2u);
    EXPECT_EQ(img.starting_hooks().size(), 1u);
    EXPECT_EQ(img.started_hooks().size(), 1u);
    EXPECT_EQ(img.stopping_hooks().size(), 1u);
}

TEST(GenericImage, StartupAttemptsBuilder) {
    GenericImage img("alpine", "3.20");
    EXPECT_EQ(img.with_startup_attempts(3).startup_attempts(), 3);

    // Values < 1 clamp to a single attempt (no retry).
    EXPECT_EQ(img.with_startup_attempts(0).startup_attempts(), 1);
    EXPECT_EQ(img.with_startup_attempts(-5).startup_attempts(), 1);
}

TEST(GenericImage, LifecycleBuildersChainOnRvalue) {
    const LifecycleHook noop = [](DockerClient&, const std::string&) {};
    const GenericImage img = GenericImage("alpine", "3.20")
                                 .with_created_hook(noop)
                                 .with_starting_hook(noop)
                                 .with_started_hook(noop)
                                 .with_stopping_hook(noop)
                                 .with_startup_attempts(2);
    EXPECT_EQ(img.created_hooks().size(), 1u);
    EXPECT_EQ(img.starting_hooks().size(), 1u);
    EXPECT_EQ(img.started_hooks().size(), 1u);
    EXPECT_EQ(img.stopping_hooks().size(), 1u);
    EXPECT_EQ(img.startup_attempts(), 2);
}

TEST(GenericImage, ToRequestSnapshotsBuilderState) {
    const LifecycleHook noop = [](DockerClient&, const std::string&) {};
    GenericImage img("redis", "7.2");
    img.with_exposed_port(tcp(6379))
        .with_env("MODE", "standalone")
        .with_cmd({"redis-server"})
        .with_label("owner", "tc")
        .with_copy_to(CopyToContainer::content("hello", "/tmp/hello.txt"))
        .with_wait(wait_for::stdout_message("Ready"))
        .with_startup_timeout(std::chrono::milliseconds(5000))
        .with_registry_auth(RegistryAuth{})
        .with_image_pull_policy(ImagePullPolicy::Always)
        .with_reuse(true)
        .with_created_hook(noop)
        .with_starting_hook(noop)
        .with_started_hook(noop)
        .with_stopping_hook(noop)
        .with_startup_attempts(3)
        // A custom substitutor pins the image reference, keeping the assert
        // independent of TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX in the environment.
        .with_image_name_substitutor([](const std::string& ref) { return "sub/" + ref; });

    const ContainerRequest req = img.to_request();

    // The create spec is fully translated.
    EXPECT_EQ(req.spec.image, "sub/redis:7.2");
    EXPECT_EQ(req.spec.cmd, (std::vector<std::string>{"redis-server"}));
    EXPECT_EQ(req.spec.env, (std::vector<std::string>{"MODE=standalone"}));
    EXPECT_EQ(req.spec.exposed_ports, (std::vector<std::string>{"6379/tcp"}));
    EXPECT_TRUE(req.spec.publish_all_ports);
    // The snapshot carries EXACTLY the builder's labels: the session/reuse
    // labels are layered on by run() (they depend on the run, not the request).
    EXPECT_EQ(req.spec.labels, (std::vector<std::pair<std::string, std::string>>{{"owner", "tc"}}));

    // The orchestration fields mirror the builders one-to-one.
    EXPECT_EQ(req.exposed_ports, (std::vector<ContainerPort>{tcp(6379)}));
    ASSERT_EQ(req.copy_to_sources.size(), 1u);
    EXPECT_EQ(req.copy_to_sources[0].target(), "/tmp/hello.txt");
    ASSERT_EQ(req.waits.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<wait_for::LogMessage>(req.waits[0]));
    EXPECT_EQ(req.startup_timeout, std::chrono::milliseconds(5000));
    EXPECT_TRUE(req.registry_auth.has_value());
    EXPECT_EQ(req.pull_policy, ImagePullPolicy::Always);
    EXPECT_TRUE(req.reuse);
    EXPECT_EQ(req.created_hooks.size(), 1u);
    EXPECT_EQ(req.starting_hooks.size(), 1u);
    EXPECT_EQ(req.started_hooks.size(), 1u);
    EXPECT_EQ(req.stopping_hooks.size(), 1u);
    EXPECT_EQ(req.startup_attempts, 3);
}

TEST(GenericImage, ExposedHostPortsDefaultEmpty) {
    const GenericImage img("alpine", "3.20");
    EXPECT_TRUE(img.exposed_host_ports().empty());
    EXPECT_TRUE(img.to_request().host_access_ports.empty());
}

TEST(GenericImage, ExposedHostPortsAccumulateAndSnapshot) {
    const ContainerRequest req = GenericImage("alpine", "3.20")
                                     .with_exposed_host_port(8080)
                                     .with_exposed_host_port(5432)
                                     .to_request();

    EXPECT_EQ(req.host_access_ports, (std::vector<std::uint16_t>{8080, 5432}));
    // The alias's ExtraHosts entry depends on the run (the sidecar's IP), so
    // the snapshot must NOT have touched the create spec.
    EXPECT_TRUE(req.spec.extra_hosts.empty());
}

TEST(GenericImage, PullPolicyOverloadsReplaceEachOther) {
    GenericImage img("redis");
    EXPECT_FALSE(img.pull_max_age().has_value()); // no budget by default

    img.with_image_pull_policy(std::chrono::hours(24));
    EXPECT_EQ(img.image_pull_policy(), ImagePullPolicy::Default); // age implies Default
    ASSERT_TRUE(img.pull_max_age().has_value());
    EXPECT_EQ(*img.pull_max_age(), std::chrono::hours(24));
    EXPECT_EQ(img.to_request().pull_max_age, std::chrono::seconds(std::chrono::hours(24)));

    // "Each call replaces the whole policy": the enum overload clears the
    // budget it did not set.
    img.with_image_pull_policy(ImagePullPolicy::Always);
    EXPECT_EQ(img.image_pull_policy(), ImagePullPolicy::Always);
    EXPECT_FALSE(img.pull_max_age().has_value());
    EXPECT_FALSE(img.to_request().pull_max_age.has_value());
}
