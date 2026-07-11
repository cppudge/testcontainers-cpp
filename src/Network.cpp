#include "testcontainers/Network.hpp"

#include <string>
#include <utility>
#include <vector>

#include "HostPortForwarding.hpp"
#include "RandomHex.hpp"
#include "Reaper.hpp"
#include "Reuse.hpp"
#include "docker/ApiMapping.hpp"
#include "testcontainers/Error.hpp"

namespace testcontainers {

Network Network::create(std::string name) {
    // Bring up the reaper first so a network created here is reaped on a crash
    // (no-op if Ryuk is disabled).
    detail::Reaper::instance().ensure_started();

    DockerClient client = DockerClient::from_environment();
    std::string id = client.create_network(name, detail::testcontainers_labels());
    return Network(std::move(client), std::move(id), std::move(name));
}

Network Network::create() { return create(detail::random_resource_name()); }

Network Network::Builder::create() const {
    // Bring up the reaper first so a network created here is reaped on a crash
    // (no-op if Ryuk is disabled).
    detail::Reaper::instance().ensure_started();

    NetworkCreateSpec spec;
    // A network always needs a name; default to a generated unique one.
    spec.name = name_.empty() ? detail::random_resource_name() : name_;
    spec.driver = driver_;
    spec.internal = internal_;
    spec.attachable = attachable_;
    spec.enable_ipv6 = enable_ipv6_;
    spec.subnet = subnet_;
    spec.gateway = gateway_;
    spec.ipam_pools = ipam_pools_;
    spec.options = options_;
    spec.labels = labels_;

    // Reuse is a safety-gated opt-in, like container reuse: it only activates
    // when enabled globally; otherwise with_reuse degrades to a normal
    // (session-labeled, reaped) network.
    if (reuse_ && detail::reuse_enabled()) {
        if (name_.empty()) {
            throw DockerError("Network reuse needs a fixed name: combine with_reuse() with "
                              "with_name(...) - a generated name would never match across runs");
        }
        // Canonical config for the hash: the create body with the USER labels
        // only (the reuse labels layered on below must not feed the hash).
        const std::string hash = detail::reuse_hash(docker::build_network_create_body(spec).dump());

        // Find-before-create, by exact name: the daemon happily creates
        // duplicate network names (no 409), and a same-named duplicate would
        // make joining by name ambiguous. The daemon's name filter matches
        // substrings, hence the exact comparison on the results.
        DockerClient client = DockerClient::from_environment();
        bool name_taken = false;
        for (const NetworkInspect& candidate : client.list_networks({{"name", spec.name}})) {
            if (candidate.name != spec.name) {
                continue;
            }
            name_taken = true;
            const auto label = candidate.labels.find(detail::reuse_hash_label());
            if (label != candidate.labels.end() && label->second == hash) {
                // Adopt it: a persistent handle (removal stays external, like
                // every reuse handle).
                Network adopted(std::move(client), candidate.id, spec.name);
                adopted.keep();
                return adopted;
            }
        }
        if (name_taken) {
            throw DockerError("with_reuse: network '" + spec.name +
                              "' already exists with a different configuration (or is not a "
                              "reuse network); creating another would make the name ambiguous "
                              "- remove it, or change the config back / pick another name");
        }

        // No match: create a NEW reuse network — managed-by + the reuse hash,
        // NO session label (Ryuk must not reap it).
        spec.labels.emplace_back("org.testcontainers.managed-by", "testcontainers");
        spec.labels.emplace_back(detail::reuse_hash_label(), hash);
        std::string id = client.create_network(spec);
        Network fresh(std::move(client), std::move(id), std::move(spec.name));
        fresh.keep();
        return fresh;
    }

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
    if (!remove_on_drop_) {
        return; // kept: the network (and its endpoints) is the caller's now
    }
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
