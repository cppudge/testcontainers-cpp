#include "testcontainers/Network.hpp"

#include <string>

#include "HostPortForwarding.hpp"
#include "RandomHex.hpp"
#include "Reaper.hpp"

namespace testcontainers {

namespace {

/// Generate a unique-ish network name like "tc-1a2b3c4d5e6f7a8b".
std::string random_network_name() { return "tc-" + detail::random_hex(16); }

} // namespace

Network Network::create(std::string name) {
    // Bring up the reaper first so a network created here is reaped on a crash
    // (no-op if Ryuk is disabled).
    detail::Reaper::instance().ensure_started();

    DockerClient client = DockerClient::from_environment();
    std::string id = client.create_network(name, detail::testcontainers_labels());
    return Network(std::move(client), std::move(id), std::move(name));
}

Network Network::create() { return create(random_network_name()); }

Network Network::Builder::create() const {
    // Bring up the reaper first so a network created here is reaped on a crash
    // (no-op if Ryuk is disabled).
    detail::Reaper::instance().ensure_started();

    NetworkCreateSpec spec;
    // A network always needs a name; default to a generated unique one.
    spec.name = name_.empty() ? random_network_name() : name_;
    spec.driver = driver_;
    spec.internal = internal_;
    spec.attachable = attachable_;
    spec.enable_ipv6 = enable_ipv6_;
    spec.subnet = subnet_;
    spec.gateway = gateway_;
    spec.options = options_;
    spec.labels = labels_;
    // Tag the network so Ryuk (and tooling) can find it: managed-by + session.
    // Merged on top of the user labels so the network is always reaped.
    for (const auto& label : detail::testcontainers_labels()) {
        spec.labels.push_back(label);
    }

    DockerClient client = DockerClient::from_environment();
    std::string id = client.create_network(spec);
    return Network(std::move(client), std::move(id), std::move(spec.name));
}

void Network::remove() { drop(); }

NetworkInspect Network::inspect() const { return client_.inspect_network(id_); }

std::string Network::inspect_raw() const { return client_.inspect_network_raw(id_); }

NetworkInspect Network::inspect(const std::string& id_or_name) {
    return DockerClient::from_environment().inspect_network(id_or_name);
}

void Network::connect(const std::string& container_id,
                      const std::vector<std::string>& aliases) const {
    client_.connect_network(id_, container_id, aliases);
}

void Network::drop() noexcept {
    if (dropped_) {
        return;
    }
    dropped_ = true;
    try {
        // If host-port exposure joined its sshd sidecar to this network, detach
        // it first — a network with active endpoints cannot be removed.
        detail::HostPortForwarder::instance().release_network(client_, name_);
        client_.remove_network(id_);
    } catch (...) {
        // Best-effort: a teardown failure must never propagate (esp. from the
        // destructor).
    }
}

} // namespace testcontainers
