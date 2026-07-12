#include <gtest/gtest.h>

#include <string>

#include "testcontainers/ExecResult.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/modules/RabbitMQ.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Linux-containers Docker daemon):
//   RabbitMQModule.DefaultsStartAndUrls - the ordered readiness pair passes (log first, diagnostics exec second — an early exec would brick the boot); amqp_url() carries guest/guest with NO path (default vhost "/"), management_url() has the http shape, and check_running exits 0.
//   RabbitMQModule.CustomCredentialsAndVhost - the configured account authenticates over the broker's auth backend, the vhost exists, and amqp_url() ends /orders.
//   RabbitMQModule.DefinitionsPreloadWithSeededAccount - a definitions file with NO users section still boots a broker where the queue exists AND the configured account authenticates (the module's seed file compensates for RabbitMQ skipping all default provisioning under load_definitions).
//   RabbitMQModule.PluginEnabled - with_plugin("rabbitmq_shovel") is enabled once the broker is ready (additive — the management plugin survives).
//   RabbitMQModule.ManagementHttpServes - a customizer-added HTTP wait against the published 15672 proves the management listener serves through the mapped port.

using namespace testcontainers;
using modules::RabbitMQContainer;
using modules::RabbitMQImage;

// Requires a Linux-containers daemon; skipped otherwise.
class RabbitMQModule : public tcit::LinuxEngineTest {};

TEST_F(RabbitMQModule, DefaultsStartAndUrls) {
    const RabbitMQContainer rmq = RabbitMQImage().start();

    // Default vhost "/": no path segment in the URI (an absent path means
    // the client's default vhost; the explicit /%2F trips naive parsers).
    EXPECT_EQ(rmq.amqp_url(),
              "amqp://guest:guest@" + rmq.host() + ":" + std::to_string(rmq.amqp_port()));
    EXPECT_EQ(rmq.management_url(),
              "http://" + rmq.host() + ":" + std::to_string(rmq.management_port()));
    EXPECT_EQ(rmq.vhost(), "/");

    const ExecResult running =
        rmq.container().exec({"rabbitmq-diagnostics", "-q", "check_running"});
    EXPECT_EQ(running.exit_code, 0) << running.stderr_data;
}

TEST_F(RabbitMQModule, CustomCredentialsAndVhost) {
    const RabbitMQContainer rmq =
        RabbitMQImage().with_username("app").with_password("s3cret").with_vhost("orders").start();

    // authenticate_user drives the broker's real auth backend.
    const ExecResult auth =
        rmq.container().exec({"rabbitmqctl", "authenticate_user", "app", "s3cret"});
    EXPECT_EQ(auth.exit_code, 0) << auth.stderr_data;

    const ExecResult vhosts = rmq.container().exec({"rabbitmqctl", "-q", "list_vhosts"});
    EXPECT_EQ(vhosts.exit_code, 0);
    EXPECT_NE(vhosts.stdout_data.find("orders"), std::string::npos);

    const std::string url = rmq.amqp_url();
    EXPECT_NE(url.find("app:s3cret@"), std::string::npos);
    EXPECT_EQ(url.substr(url.size() - 7), "/orders");
}

TEST_F(RabbitMQModule, DefinitionsPreloadWithSeededAccount) {
    // NO "users" section on purpose: under load_definitions RabbitMQ skips
    // ALL default provisioning, so without the module's seed file this
    // broker would boot with zero users (guest included).
    const RabbitMQContainer rmq =
        RabbitMQImage()
            .with_username("app")
            .with_password("s3cret")
            .with_definitions_json(R"({"queues":[{"name":"preloaded","vhost":"/",)"
                                   R"("durable":true,"auto_delete":false,"arguments":{}}]})")
            .start();

    const ExecResult queues =
        rmq.container().exec({"rabbitmqctl", "-q", "list_queues", "-p", "/", "name"});
    EXPECT_EQ(queues.exit_code, 0) << queues.stderr_data;
    EXPECT_NE(queues.stdout_data.find("preloaded"), std::string::npos);

    const ExecResult auth =
        rmq.container().exec({"rabbitmqctl", "authenticate_user", "app", "s3cret"});
    EXPECT_EQ(auth.exit_code, 0) << auth.stderr_data;
}

TEST_F(RabbitMQModule, PluginEnabled) {
    const RabbitMQContainer rmq = RabbitMQImage().with_plugin("rabbitmq_shovel").start();

    const ExecResult shovel =
        rmq.container().exec({"rabbitmq-plugins", "is_enabled", "rabbitmq_shovel"});
    EXPECT_EQ(shovel.exit_code, 0) << shovel.stderr_data;

    // Additive: the image's own management plugin survived the enable.
    const ExecResult management =
        rmq.container().exec({"rabbitmq-plugins", "is_enabled", "rabbitmq_management"});
    EXPECT_EQ(management.exit_code, 0) << management.stderr_data;
}

TEST_F(RabbitMQModule, ManagementHttpServes) {
    // The customizer-added wait lands AFTER the module's ordered pair (the
    // safe position); the UI root serves 200 unauthenticated, so start()
    // returning proves the management listener through the published port.
    const RabbitMQContainer rmq = RabbitMQImage()
                                      .with_customizer([](GenericImage& generic) {
                                          generic.with_wait(wait_for::http("/", tcp(15672), 200));
                                      })
                                      .start();

    EXPECT_GT(rmq.management_port(), 0);
}
