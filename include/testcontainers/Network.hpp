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
/// removal happens exactly once; `keep()` releases removal ownership (the
/// network then outlives the handle). Containers placed on the same network
/// (via `GenericImage::with_network`) can resolve each other by container name.
class Network {
public:
    /// A copyable builder for a richer user-defined network: driver, internal /
    /// attachable / IPv6 flags, IPAM address pools, driver options, and
    /// labels. Defined in std types only (no Boost/json leak through the header).
    ///
    /// The `with_*` setters mutate in place and return `*this` by reference; a
    /// single unqualified overload chains on both a named lvalue and a temporary.
    /// `create()` assembles the spec, runs the Reaper hook, and creates the network.
    class Builder {
    public:
        Builder& with_name(std::string name) {
            name_ = std::move(name);
            return *this;
        }

        Builder& with_driver(std::string driver) {
            driver_ = std::move(driver);
            return *this;
        }

        Builder& with_internal(bool internal = true) {
            internal_ = internal;
            return *this;
        }

        Builder& with_attachable(bool attachable = true) {
            attachable_ = attachable;
            return *this;
        }

        Builder& with_enable_ipv6(bool enable = true) {
            enable_ipv6_ = enable;
            return *this;
        }

        /// IPAM subnet in CIDR form, e.g. "172.31.250.0/24".
        Builder& with_subnet(std::string cidr) {
            subnet_ = std::move(cidr);
            return *this;
        }

        /// IPAM gateway address (requires a subnet to take effect).
        Builder& with_gateway(std::string ip) {
            gateway_ = std::move(ip);
            return *this;
        }

        /// Add an IPAM address pool (an `IPAM.Config` entry: subnet, allocation
        /// range, gateway, auxiliary addresses; empty fields are omitted). Pools
        /// land after the `with_subnet`/`with_gateway` shorthand pool when that
        /// is set. More than one IPv4 pool needs a driver that supports it — the
        /// Linux bridge driver takes one IPv4 pool, plus one IPv6 pool with
        /// `with_enable_ipv6`.
        Builder& with_ipam_pool(NetworkIpamPool pool) {
            ipam_pools_.push_back(std::move(pool));
            return *this;
        }

        /// Add a driver option (`Options`). Add several to set multiple options.
        Builder& with_option(std::string key, std::string value) {
            options_.emplace_back(std::move(key), std::move(value));
            return *this;
        }

        /// Add a network label (`Labels`). The testcontainers session labels are
        /// always merged in by `create()` for Ryuk reaping.
        Builder& with_label(std::string key, std::string value) {
            labels_.emplace_back(std::move(key), std::move(value));
            return *this;
        }

        /// Enable network reuse. When reuse is also enabled globally
        /// (`testcontainers.reuse.enable=true` in ~/.testcontainers.properties
        /// or `TESTCONTAINERS_REUSE_ENABLE=true`), `create()` first looks for
        /// an existing network with this exact name whose reuse-hash label
        /// matches this configuration and ADOPTS it instead of creating a new
        /// one; either way the returned handle is persistent (it does NOT
        /// remove the network on destruction, and the network is NOT
        /// Ryuk-reaped, so it survives across runs — clean up externally, e.g.
        /// `docker network rm` after a `label=org.testcontainers.reuse.hash`
        /// sweep). Requires `with_name` (a generated name would never match
        /// across runs). If the name is already taken by a network with a
        /// DIFFERENT configuration, `create()` throws instead of making a
        /// same-named duplicate (Docker does not enforce unique network names,
        /// and a duplicate makes joining by name ambiguous). When reuse is not
        /// enabled globally this is a no-op: `create()` behaves exactly like a
        /// normal (session-labeled, reaped) network.
        Builder& with_reuse(bool reuse = true) {
            reuse_ = reuse;
            return *this;
        }

        /// Create the network from the configured options, returning the handle.
        /// With `with_reuse` active this is find-or-create (see there).
        Network create() const;

    private:
        std::string name_;
        std::optional<std::string> driver_;
        bool internal_ = false;
        bool attachable_ = false;
        bool enable_ipv6_ = false;
        std::optional<std::string> subnet_;
        std::optional<std::string> gateway_;
        std::vector<NetworkIpamPool> ipam_pools_;
        std::vector<std::pair<std::string, std::string>> options_;
        std::vector<std::pair<std::string, std::string>> labels_;
        bool reuse_ = false;
    };

    /// Start a builder for a richer network (driver, IPAM, options, labels).
    static Builder builder() { return Builder{}; }

    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    Network(Network&& other) noexcept
        : client_(std::move(other.client_)), id_(std::move(other.id_)),
          name_(std::move(other.name_)), dropped_(other.dropped_),
          remove_on_drop_(other.remove_on_drop_) {
        other.dropped_ = true; // the moved-from handle owns nothing
    }

    Network& operator=(Network&& other) noexcept {
        if (this != &other) {
            drop();
            client_ = std::move(other.client_);
            id_ = std::move(other.id_);
            name_ = std::move(other.name_);
            dropped_ = other.dropped_;
            remove_on_drop_ = other.remove_on_drop_;
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

    /// True when this handle will NOT remove the network on destruction
    /// (after `keep()`).
    bool is_persistent() const noexcept { return !remove_on_drop_; }

    /// A structured snapshot of this network (`GET /networks/{id}`): driver,
    /// flags, IPAM pools, options, labels, and the currently attached
    /// containers. Throws DockerError if the network is gone.
    NetworkInspect inspect() const;

    /// The RAW inspect JSON body (`GET /networks/{id}`), so callers can read
    /// any field `NetworkInspect` does not model. Throws DockerError if the
    /// network is gone.
    std::string inspect_raw() const;

    /// A structured snapshot of an arbitrary network by name or id, without a
    /// Network handle (the daemon accepts both forms). Connects via
    /// `DockerClient::from_environment()`. Throws DockerError if no such
    /// network exists (NotFoundError) or the daemon cannot be reached.
    static NetworkInspect inspect(const std::string& id_or_name);

    /// Keep the network alive past this handle: from here on neither
    /// destruction nor `remove()` removes it (`is_persistent()` reports true
    /// afterwards) — removing the network becomes the caller's responsibility
    /// (e.g. `DockerClient::remove_network` or `docker network rm`).
    /// `keep(false)` re-arms removal — handy for forwarding a "keep my
    /// resources" debug flag in one call instead of an `if`.
    ///
    /// Ryuk still applies on Linux engines: a created network carries the
    /// session label, so the reaper removes it shortly after the test process
    /// exits — keep() only protects it from THIS process's teardown. For a
    /// network that must outlive the process, disable the reaper
    /// (TESTCONTAINERS_RYUK_DISABLED). (No reaper runs against a
    /// Windows-containers engine — a kept network there stays until you
    /// remove it.)
    void keep(bool keep = true) noexcept { remove_on_drop_ = !keep; }

    /// Explicitly remove the network now. Idempotent; after this the destructor
    /// does nothing. On a kept handle (after `keep()`) this releases ownership
    /// without removing the network.
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
    bool remove_on_drop_ = true; ///< false after keep()
};

} // namespace testcontainers
