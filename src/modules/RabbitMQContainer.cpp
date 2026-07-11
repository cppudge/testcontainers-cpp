#include "testcontainers/modules/RabbitMQContainer.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "testcontainers/ConnectionString.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers::modules {

namespace {

constexpr const char* kDefinitionsDir = "/etc/rabbitmq/definitions.d";

/// The account/vhost seed imported ALONGSIDE user definition files: RabbitMQ
/// skips all default-account provisioning (RABBITMQ_DEFAULT_*) when
/// definitions are loaded at boot — without this file, user definitions with
/// no "users" entry would leave the broker with zero users. Sorted FIRST
/// (0010- < 05NN-), so a user file declaring the same objects wins.
std::string seed_definitions_json(const std::string& username, const std::string& password,
                                  const std::string& vhost) {
    nlohmann::json seed;
    // Plaintext "password" fields: the importer hashes them on load —
    // fixture-grade credentials, same exposure class as the env trio.
    seed["users"] = nlohmann::json::array(
        {{{"name", username}, {"password", password}, {"tags", "administrator"}}});
    seed["vhosts"] = nlohmann::json::array({{{"name", vhost}}});
    seed["permissions"] = nlohmann::json::array({{{"user", username},
                                                  {"vhost", vhost},
                                                  {"configure", ".*"},
                                                  {"write", ".*"},
                                                  {"read", ".*"}}});
    return seed.dump();
}

} // namespace

RabbitMQContainer::RabbitMQContainer()
    : image_(GenericImage::from_reference(std::string(kDefaultImage))) {
    // Both ports every test needs; extra listeners (MQTT/STOMP/Prometheus)
    // stay unpublished unless a customizer exposes them.
    image_.with_exposed_port(tcp(kAmqpPort)).with_exposed_port(tcp(kManagementPort));
}

RabbitMQContainer& RabbitMQContainer::with_image(const std::string& reference) {
    image_.with_image(reference);
    return *this;
}

std::string RabbitMQContainer::next_definitions_target() const {
    std::string digits = std::to_string(500 + definitions_.size());
    if (digits.size() < 4) {
        digits.insert(0, 4 - digits.size(), '0');
    }
    return std::string(kDefinitionsDir) + "/" + digits + "-definitions.json";
}

RabbitMQContainer& RabbitMQContainer::with_definitions(std::filesystem::path host_json) {
    const std::string name = host_json.filename().string();
    if (name.size() <= 5 || !std::string_view(name).ends_with(".json")) {
        throw Error("definitions file \"" + name +
                    "\" must be named *.json (the broker imports the definitions directory "
                    "as JSON)");
    }
    if (definitions_.size() >= 9500) {
        throw Error("too many definitions files (the 05NN ordering prefix is four digits)");
    }
    const std::string target = next_definitions_target();
    definitions_.push_back(CopyToContainer::host_file(std::move(host_json), target));
    return *this;
}

RabbitMQContainer& RabbitMQContainer::with_definitions_json(std::string json) {
    if (definitions_.size() >= 9500) {
        throw Error("too many definitions files (the 05NN ordering prefix is four digits)");
    }
    const std::string target = next_definitions_target();
    definitions_.push_back(CopyToContainer::content(std::move(json), target));
    return *this;
}

// Out of line so the header needs no Network definition.
RabbitMQContainer& RabbitMQContainer::with_network(const Network& network) {
    image_.with_network(network);
    return *this;
}

GenericImage RabbitMQContainer::to_generic() const {
    // Fail fast, before any daemon contact.
    if (username_.empty()) {
        throw Error("RabbitMQ username must not be empty (with_username)");
    }
    if (password_.empty()) {
        // Verified live: the account gets created, but the broker's internal
        // auth backend prohibits blank-password logins outright — the handle
        // could never authenticate.
        throw Error("RabbitMQ password must not be empty (with_password): the broker "
                    "prohibits blank-password logins on its internal auth backend");
    }
    if (vhost_.empty()) {
        throw Error("RabbitMQ vhost must not be empty (with_vhost); the broker's default "
                    "vhost is \"/\"");
    }

    // Render into a COPY: repeated to_generic()/start() calls must never
    // accumulate state in the config.
    GenericImage generic = image_;

    // Always set explicitly, so inspect shows the truth. With definitions
    // configured the broker ignores these — the seed file below carries the
    // same account, so both provisioning paths agree.
    generic.with_env("RABBITMQ_DEFAULT_USER", username_);
    generic.with_env("RABBITMQ_DEFAULT_PASS", password_);
    generic.with_env("RABBITMQ_DEFAULT_VHOST", vhost_);

    if (!definitions_.empty()) {
        generic.with_copy_to(CopyToContainer::content(
            seed_definitions_json(username_, password_, vhost_),
            std::string(kDefinitionsDir) + "/0010-testcontainers-seed.json"));
        for (const CopyToContainer& definitions : definitions_) {
            generic.with_copy_to(definitions);
        }
        // The image reads /etc/rabbitmq/conf.d/*.conf (its own 10-defaults
        // lives there); this drop-in points the broker at the directory.
        generic.with_copy_to(
            CopyToContainer::content(std::string("load_definitions = ") + kDefinitionsDir + "\n",
                                     "/etc/rabbitmq/conf.d/20-testcontainers-definitions.conf"));
    }

    // ORDERED waits — load-bearing. The log wait never execs; the diagnostics
    // exec runs only after it, by which time the server's own Erlang VM has
    // written its cookie. An exec in the first ~2s of boot runs as root and
    // creates a root-owned 0400 cookie the uid-999 server then cannot read —
    // the node dies and nothing inside that container can recover it.
    generic.with_wait(wait_for::log("Server startup complete"));
    wait_for::Command probe;
    probe.cmd = {"rabbitmq-diagnostics", "-q", "check_port_connectivity"};
    // Each attempt boots an Erlang CLI VM (~0.5s at ready state) — the 200ms
    // default poll would be pure churn.
    probe.poll_interval = std::chrono::seconds(1);
    generic.with_wait(probe);

    if (!plugins_.empty()) {
        // The plugin list lives inside the hook lambda, invisible to the
        // reuse hash — this label pushes it into the create body (order-
        // normalized), so a changed plugin set creates a fresh container
        // instead of silently adopting one without the new plugins.
        std::vector<std::string> sorted_plugins = plugins_;
        std::sort(sorted_plugins.begin(), sorted_plugins.end());
        std::string label;
        for (const std::string& plugin : sorted_plugins) {
            if (!label.empty()) {
                label += ',';
            }
            label += plugin;
        }
        generic.with_label("org.testcontainers.rabbitmq.plugins", label);

        const std::vector<std::string> plugins = plugins_;
        generic.with_started_hook([plugins](DockerClient& client, const std::string& id) {
            // Additive on purpose: overwriting enabled_plugins at boot would
            // silently drop the image's own management/prometheus plugins.
            std::vector<std::string> cmd{"rabbitmq-plugins", "enable", "--quiet"};
            cmd.insert(cmd.end(), plugins.begin(), plugins.end());
            const ExecResult res = client.exec(id, cmd);
            if (res.exit_code != 0) {
                throw DockerError(
                    "rabbitmq-plugins enable failed (exit " + std::to_string(res.exit_code) +
                        "): " + (res.stderr_data.empty() ? res.stdout_data : res.stderr_data),
                    std::nullopt, id);
            }
        });
    }

    // Customizers last: what the escape hatch sets wins over the module's
    // rendering (waits added here land AFTER the ordered pair — the safe
    // position).
    for (const auto& customize : customizers_) {
        customize(generic);
    }
    return generic;
}

StartedRabbitMQ RabbitMQContainer::start() const {
    return StartedRabbitMQ(to_generic().start(), username_, password_, vhost_);
}

std::string StartedRabbitMQ::amqp_url() const {
    ConnectionString url("amqp");
    url.with_user(username_).with_password(password_).with_host(host_).with_port(amqp_port_);
    if (vhost_ != "/") {
        // "/" emits no path: an absent path means the client's default vhost
        // in every mainstream client, and unlike the explicit /%2F spelling
        // it survives URI parsers that skip percent-decoding.
        url.with_database(vhost_);
    }
    return url.to_string();
}

std::string StartedRabbitMQ::management_url() const {
    ConnectionString url("http");
    url.with_host(host_).with_port(management_port_);
    return url.to_string();
}

} // namespace testcontainers::modules
