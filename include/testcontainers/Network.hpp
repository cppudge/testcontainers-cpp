#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers {

/// A RAII handle to a user-defined Docker network.
///
/// Move-only: it owns a real external resource and removes the network on
/// destruction (best-effort, exceptions swallowed). Copying is deleted so the
/// removal happens exactly once. Containers placed on the same network (via
/// `GenericImage::with_network`) can resolve each other by container name.
class Network {
public:
    /// A copyable builder for a richer user-defined network: driver, internal /
    /// attachable / IPv6 flags, an IPAM subnet+gateway, driver options, and
    /// labels. Defined in std types only (no Boost/json leak through the header).
    ///
    /// The `with_*` setters mutate in place and return `*this` (ref-qualified), so
    /// the chain works on both lvalues and rvalues. `create()` assembles the spec,
    /// runs the Reaper hook, and creates the network.
    class Builder {
    public:
        Builder& with_name(std::string name) & {
            name_ = std::move(name);
            return *this;
        }
        Builder&& with_name(std::string name) && {
            name_ = std::move(name);
            return std::move(*this);
        }

        Builder& with_driver(std::string driver) & {
            driver_ = std::move(driver);
            return *this;
        }
        Builder&& with_driver(std::string driver) && {
            driver_ = std::move(driver);
            return std::move(*this);
        }

        Builder& with_internal(bool internal = true) & {
            internal_ = internal;
            return *this;
        }
        Builder&& with_internal(bool internal = true) && {
            internal_ = internal;
            return std::move(*this);
        }

        Builder& with_attachable(bool attachable = true) & {
            attachable_ = attachable;
            return *this;
        }
        Builder&& with_attachable(bool attachable = true) && {
            attachable_ = attachable;
            return std::move(*this);
        }

        Builder& with_enable_ipv6(bool enable = true) & {
            enable_ipv6_ = enable;
            return *this;
        }
        Builder&& with_enable_ipv6(bool enable = true) && {
            enable_ipv6_ = enable;
            return std::move(*this);
        }

        /// IPAM subnet in CIDR form, e.g. "172.31.250.0/24".
        Builder& with_subnet(std::string cidr) & {
            subnet_ = std::move(cidr);
            return *this;
        }
        Builder&& with_subnet(std::string cidr) && {
            subnet_ = std::move(cidr);
            return std::move(*this);
        }

        /// IPAM gateway address (requires a subnet to take effect).
        Builder& with_gateway(std::string ip) & {
            gateway_ = std::move(ip);
            return *this;
        }
        Builder&& with_gateway(std::string ip) && {
            gateway_ = std::move(ip);
            return std::move(*this);
        }

        /// Add a driver option (`Options`). Add several to set multiple options.
        Builder& with_option(std::string key, std::string value) & {
            options_.emplace_back(std::move(key), std::move(value));
            return *this;
        }
        Builder&& with_option(std::string key, std::string value) && {
            options_.emplace_back(std::move(key), std::move(value));
            return std::move(*this);
        }

        /// Add a network label (`Labels`). The testcontainers session labels are
        /// always merged in by `create()` for Ryuk reaping.
        Builder& with_label(std::string key, std::string value) & {
            labels_.emplace_back(std::move(key), std::move(value));
            return *this;
        }
        Builder&& with_label(std::string key, std::string value) && {
            labels_.emplace_back(std::move(key), std::move(value));
            return std::move(*this);
        }

        /// Create the network from the configured options, returning the handle.
        Network create() const;

    private:
        std::string name_;
        std::optional<std::string> driver_;
        bool internal_ = false;
        bool attachable_ = false;
        bool enable_ipv6_ = false;
        std::optional<std::string> subnet_;
        std::optional<std::string> gateway_;
        std::vector<std::pair<std::string, std::string>> options_;
        std::vector<std::pair<std::string, std::string>> labels_;
    };

    /// Start a builder for a richer network (driver, IPAM, options, labels).
    static Builder builder() { return Builder{}; }

    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    Network(Network&& other) noexcept
        : client_(std::move(other.client_)), id_(std::move(other.id_)),
          name_(std::move(other.name_)), dropped_(other.dropped_) {
        other.dropped_ = true; // the moved-from handle owns nothing
    }

    Network& operator=(Network&& other) noexcept {
        if (this != &other) {
            drop();
            client_ = std::move(other.client_);
            id_ = std::move(other.id_);
            name_ = std::move(other.name_);
            dropped_ = other.dropped_;
            other.dropped_ = true;
        }
        return *this;
    }

    /// Removes the network unless it was already explicitly removed or
    /// moved-from. Never throws.
    ~Network() { drop(); }

    /// Create a network with the given name.
    static Network create(std::string name);

    /// Create a network with a generated unique name (`tc-<random hex>`).
    static Network create();

    /// The network's name (used by containers to join it / resolve peers).
    const std::string& name() const noexcept { return name_; }

    /// The network's id.
    const std::string& id() const noexcept { return id_; }

    /// Explicitly remove the network now. Idempotent; after this the destructor
    /// does nothing.
    void remove();

    /// Connect an already-running container to this network, optionally giving it
    /// DNS aliases on this network. Lets a container join after it was started
    /// (e.g. without `GenericImage::with_network`).
    void connect(const std::string& container_id,
                 const std::vector<std::string>& aliases = {}) const;

private:
    Network(DockerClient client, std::string id, std::string name)
        : client_(std::move(client)), id_(std::move(id)), name_(std::move(name)) {}

    /// Best-effort remove, swallowing any error. Marks the handle dropped.
    void drop() noexcept;

    // Mutable for the same reason as Container: the client is a stateless host
    // config that opens a fresh connection per call.
    mutable DockerClient client_;
    std::string id_;
    std::string name_;
    bool dropped_ = false;
};

} // namespace testcontainers
