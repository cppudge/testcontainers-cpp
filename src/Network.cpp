#include "testcontainers/Network.hpp"

#include <array>
#include <random>
#include <string>

#include "Reaper.hpp"

namespace testcontainers {

namespace {

/// Generate a unique-ish network name like "tc-1a2b3c4d5e6f7a8b".
std::string random_network_name() {
    static constexpr char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    std::array<char, 16> buf{};
    for (char& c : buf) {
        c = hex[dist(rd)];
    }
    return "tc-" + std::string(buf.data(), buf.size());
}

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
        client_.remove_network(id_);
    } catch (...) {
        // Best-effort: a teardown failure must never propagate (esp. from the
        // destructor).
    }
}

} // namespace testcontainers
