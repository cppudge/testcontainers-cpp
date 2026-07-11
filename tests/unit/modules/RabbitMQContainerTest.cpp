#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/modules/RabbitMQContainer.hpp"

// Tests in this file (unit; no Docker daemon — the module's rendering rules
// via to_generic()):
//   RabbitMQModuleConfig.DefaultRendersEnvPortsAndOrderedWaits - the default config renders the pinned image, exposes 5672 + 15672, sets the guest/guest/"/" env trio, and installs the ORDERED readiness pair: the log wait first, the diagnostics exec second (an exec that fires during early boot bricks the broker).
//   RabbitMQModuleConfig.CredentialsAndVhostRender - with_username/with_password/with_vhost land in the RABBITMQ_DEFAULT_* trio.
//   RabbitMQModuleConfig.DefinitionsSynthesizeSeedAndConfDropIn - with_definitions_json stages the user file after the module's seed (which carries the configured account/vhost), plus the load_definitions conf drop-in; without definitions none of the three copies exist.
//   RabbitMQModuleConfig.DefinitionsHostFileValidatesJsonExtension - a non-.json host file throws at registration (the broker imports the directory as JSON).
//   RabbitMQModuleConfig.PluginsRenderStartedHook - with_plugin registers exactly one started hook plus an order-normalized reuse-visibility label; no plugins, no hook, no label.
//   RabbitMQModuleConfig.ValidationFailsFast - empty username, password (the broker prohibits blank-password logins), or vhost throws Error at render.
//   RabbitMQModuleConfig.CustomizerRunsLastAndWins - a customizer sees the rendered builder (ordered waits in place) and its settings win.
//   RabbitMQModuleConfig.RenderingIsIdempotent - repeated to_generic() calls render equal env/copies/waits (the seed is not re-appended).

using namespace testcontainers;
using modules::RabbitMQContainer;

namespace {

std::string env_last_value(const GenericImage& generic, const std::string& key) {
    std::string value;
    for (const auto& [k, v] : generic.env()) {
        if (k == key) {
            value = v;
        }
    }
    return value;
}

} // namespace

TEST(RabbitMQModuleConfig, DefaultRendersEnvPortsAndOrderedWaits) {
    const GenericImage generic = RabbitMQContainer().to_generic();

    EXPECT_EQ(generic.image(), "rabbitmq");
    EXPECT_EQ(generic.tag(), "3.13-management");
    ASSERT_EQ(generic.exposed_ports().size(), 2u);
    EXPECT_EQ(generic.exposed_ports()[0], tcp(5672));
    EXPECT_EQ(generic.exposed_ports()[1], tcp(15672));

    EXPECT_EQ(env_last_value(generic, "RABBITMQ_DEFAULT_USER"), "guest");
    EXPECT_EQ(env_last_value(generic, "RABBITMQ_DEFAULT_PASS"), "guest");
    EXPECT_EQ(env_last_value(generic, "RABBITMQ_DEFAULT_VHOST"), "/");

    // The ORDER is the load-bearing part: log first (never execs), the
    // diagnostics exec only after the server wrote its own Erlang cookie.
    ASSERT_EQ(generic.waits().size(), 2u);
    const auto* log = std::get_if<wait_for::LogMessage>(&generic.waits()[0]);
    ASSERT_NE(log, nullptr);
    EXPECT_EQ(log->text, "Server startup complete");
    const auto* command = std::get_if<wait_for::Command>(&generic.waits()[1]);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->cmd,
              (std::vector<std::string>{"rabbitmq-diagnostics", "-q", "check_port_connectivity"}));
    EXPECT_EQ(command->poll_interval, std::chrono::seconds(1));

    EXPECT_TRUE(generic.copy_to_sources().empty()); // no definitions, no plumbing
    EXPECT_TRUE(generic.started_hooks().empty());   // no plugins, no hook
}

TEST(RabbitMQModuleConfig, CredentialsAndVhostRender) {
    const GenericImage generic = RabbitMQContainer()
                                     .with_username("app")
                                     .with_password("s3cret")
                                     .with_vhost("orders")
                                     .to_generic();

    EXPECT_EQ(env_last_value(generic, "RABBITMQ_DEFAULT_USER"), "app");
    EXPECT_EQ(env_last_value(generic, "RABBITMQ_DEFAULT_PASS"), "s3cret");
    EXPECT_EQ(env_last_value(generic, "RABBITMQ_DEFAULT_VHOST"), "orders");
}

TEST(RabbitMQModuleConfig, DefinitionsSynthesizeSeedAndConfDropIn) {
    RabbitMQContainer cfg;
    cfg.with_username("app").with_password("s3cret").with_definitions_json(
        R"({"queues":[{"name":"q1","vhost":"/","durable":true}]})");

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.copy_to_sources().size(), 3u);
    // The seed sorts FIRST in the definitions directory, so user files win
    // on conflicting objects.
    EXPECT_EQ(generic.copy_to_sources()[0].target(),
              "/etc/rabbitmq/definitions.d/0010-testcontainers-seed.json");
    // Parse rather than substring-match: the importer needs this exact shape
    // (arrays of objects), which substrings could not pin.
    const nlohmann::json seed = nlohmann::json::parse(generic.copy_to_sources()[0].bytes());
    ASSERT_TRUE(seed.at("users").is_array());
    EXPECT_EQ(seed.at("users").at(0).at("name"), "app");
    EXPECT_EQ(seed.at("users").at(0).at("password"), "s3cret");
    EXPECT_EQ(seed.at("users").at(0).at("tags"), "administrator");
    EXPECT_EQ(seed.at("vhosts").at(0).at("name"), "/");
    EXPECT_EQ(seed.at("permissions").at(0).at("user"), "app");
    EXPECT_EQ(seed.at("permissions").at(0).at("configure"), ".*");
    EXPECT_EQ(generic.copy_to_sources()[1].target(),
              "/etc/rabbitmq/definitions.d/0500-definitions.json");
    EXPECT_NE(generic.copy_to_sources()[1].bytes().find("q1"), std::string::npos);
    EXPECT_EQ(generic.copy_to_sources()[2].target(),
              "/etc/rabbitmq/conf.d/20-testcontainers-definitions.conf");
    EXPECT_EQ(generic.copy_to_sources()[2].bytes(),
              "load_definitions = /etc/rabbitmq/definitions.d\n");
}

TEST(RabbitMQModuleConfig, DefinitionsHostFileValidatesJsonExtension) {
    RabbitMQContainer cfg;
    EXPECT_THROW(cfg.with_definitions(std::filesystem::path("topology.yaml")), Error);
    EXPECT_NO_THROW(cfg.with_definitions(std::filesystem::path("fixtures/topology.json")));
}

TEST(RabbitMQModuleConfig, PluginsRenderStartedHook) {
    const GenericImage bare = RabbitMQContainer().to_generic();
    EXPECT_TRUE(bare.started_hooks().empty());
    EXPECT_TRUE(bare.labels().empty());

    // Registered out of order on purpose: the reuse-visibility label is
    // order-normalized (the plugin SET matters, not the call order).
    const GenericImage generic = RabbitMQContainer()
                                     .with_plugin("rabbitmq_stomp")
                                     .with_plugin("rabbitmq_shovel")
                                     .to_generic();
    EXPECT_EQ(generic.started_hooks().size(), 1u); // one hook enables all names
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "org.testcontainers.rabbitmq.plugins");
    EXPECT_EQ(generic.labels()[0].second, "rabbitmq_shovel,rabbitmq_stomp");
}

TEST(RabbitMQModuleConfig, ValidationFailsFast) {
    EXPECT_THROW(RabbitMQContainer().with_username("").to_generic(), Error);
    EXPECT_THROW(RabbitMQContainer().with_password("").to_generic(), Error);
    EXPECT_THROW(RabbitMQContainer().with_vhost("").to_generic(), Error);
}

TEST(RabbitMQModuleConfig, CustomizerRunsLastAndWins) {
    RabbitMQContainer cfg;
    cfg.with_customizer([](GenericImage& generic) {
        // Sees the rendered state: the ordered waits are already in place, so
        // a wait added here lands in the SAFE position (after them).
        ASSERT_EQ(generic.waits().size(), 2u);
        generic.with_label("team", "messaging");
    });

    const GenericImage generic = cfg.to_generic();
    ASSERT_EQ(generic.labels().size(), 1u);
    EXPECT_EQ(generic.labels()[0].first, "team");
}

TEST(RabbitMQModuleConfig, RenderingIsIdempotent) {
    RabbitMQContainer cfg;
    cfg.with_username("app").with_definitions_json("{}");

    const GenericImage first = cfg.to_generic();
    const GenericImage second = cfg.to_generic();
    EXPECT_EQ(first.env(), second.env());
    EXPECT_EQ(first.copy_to_sources().size(), second.copy_to_sources().size());
    EXPECT_EQ(second.waits().size(), 2u);
}
