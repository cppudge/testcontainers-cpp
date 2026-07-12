#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testcontainers/Container.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"

namespace testcontainers::modules {

class MongoDBContainer;

/// A MongoDB server for tests: a copyable, reusable description of a `mongo`
/// container that always runs as a SINGLE-NODE REPLICA SET, so sessions,
/// multi-document transactions, and change streams work out of the box.
/// `start()` boots the container, initiates the replica set, waits until the
/// node is the writable PRIMARY, and returns a `MongoDBContainer` that hands
/// out ready-to-use connection strings. The ~1–2s election cost is the whole
/// price; a one-node primary serves plain CRUD exactly like a standalone.
///
/// The server runs WITHOUT authentication: MongoDB requires a cluster
/// keyfile as soon as auth is combined with a replica set — test-hostile
/// complexity for no isolation gain on a throwaway container. Do not set
/// MONGO_INITDB_ROOT_USERNAME / MONGO_INITDB_ROOT_PASSWORD through a
/// customizer: an auth-enabled replica-set member refuses to start without a
/// keyfile.
///
/// The `with_*` builders mutate in place and return `*this` by reference, so
/// a named config can be configured incrementally and started many times.
class MongoDBImage {
public:
    /// The pinned default image. Override with `with_image`; any image with
    /// `mongosh` on the PATH works (official `mongo:5.0` or newer — older
    /// images ship only the removed legacy shell and are not supported).
    static constexpr std::string_view kDefaultImage = "mongo:7";

    /// The server port INSIDE the container. Peers on a shared docker
    /// network connect to `<alias-or-name>:kPort` (append
    /// `?directConnection=true` to their URI too); the test process itself
    /// uses `MongoDBContainer::port()` (the mapped host port) instead.
    static constexpr std::uint16_t kPort = 27017;

    /// A config ready to `start()`: image `mongo:7`, port 27017 exposed,
    /// replica set "rs0", default database "test".
    MongoDBImage();

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Override the pinned image with a full reference "name[:tag]", e.g. a
    /// hub mirror or "mongo:8".
    MongoDBImage& with_image(const std::string& reference);

    /// Rename the replica set (default "rs0"). Letters, digits, '-' and '_'
    /// only — anything else makes `start()` throw up front. The name never
    /// appears in `connection_string()` (clients connect directly), but is
    /// visible in `rs.status()` and via `MongoDBContainer::replica_set_name()`.
    MongoDBImage& with_replica_set_name(std::string name) {
        replica_set_name_ = std::move(name);
        return *this;
    }

    /// Set the database that a no-argument `connection_string()` points at
    /// (default "test"). MongoDB creates databases lazily on first write, so
    /// nothing is provisioned server-side. Use a valid MongoDB database name
    /// — the value also becomes `mongosh()`'s positional argument, where a
    /// name with ':' or '/' would parse as a connection string instead.
    MongoDBImage& with_database(std::string database) {
        database_ = std::move(database);
        return *this;
    }

    // --- Pass-throughs to the underlying builder ---

    /// Set an extra environment variable (TZ and friends). Do not set
    /// MONGO_INITDB_ROOT_USERNAME / MONGO_INITDB_ROOT_PASSWORD here: an
    /// auth-enabled replica-set member refuses to start without a cluster
    /// keyfile (see the class note), so `start()` throws up front on either
    /// key. The other MONGO_INITDB_* keys only affect initdb.d scripts,
    /// which this module deliberately does not stage.
    MongoDBImage& with_env(std::string key, std::string value) {
        image_.with_env(std::move(key), std::move(value));
        return *this;
    }

    MongoDBImage& with_label(std::string key, std::string value) {
        image_.with_label(std::move(key), std::move(value));
        return *this;
    }

    /// Join a user-defined network; peers resolve this container by
    /// name/alias at `<alias>:27017` (kPort, not the mapped host port).
    MongoDBImage& with_network(std::string network) {
        image_.with_network(std::move(network));
        return *this;
    }
    MongoDBImage& with_network(const Network& network);
    MongoDBImage& with_network_alias(std::string alias) {
        image_.with_network_alias(std::move(alias));
        return *this;
    }

    /// Enable container reuse (effective only when reuse is also enabled
    /// globally — see GenericImage::with_reuse). An adopted container's
    /// replica-set config persists in its data directory, so it is already
    /// the PRIMARY.
    MongoDBImage& with_reuse(bool reuse = true) {
        image_.with_reuse(reuse);
        return *this;
    }

    /// Budget for EACH of the two startup phases — the container-level
    /// readiness wait, and the replica-set initiation (rs.initiate + the
    /// PRIMARY election poll) that follows it — so the worst-case total is
    /// about twice this. Default 60s per phase. Image pull time does not
    /// count against it.
    MongoDBImage& with_startup_timeout(std::chrono::milliseconds timeout) {
        image_.with_startup_timeout(timeout);
        return *this;
    }

    /// Retry the whole create→start→initiate sequence up to `n` times.
    MongoDBImage& with_startup_attempts(int n) {
        image_.with_startup_attempts(n);
        return *this;
    }

    /// Register a callback that customizes the underlying `GenericImage` —
    /// the channel for every option this module does not surface (mounts,
    /// memory limits, pull policy, ...). Customizers run when the config is
    /// rendered (`start()` / `to_generic()`), in registration order, AFTER
    /// the module's own rendering. Caveats: the replica-set choreography
    /// lives in the rendered command, waits, and started hook — replacing
    /// any of them disables it — and MONGO_INITDB_* env set here breaks the
    /// boot (see the class note on auth; initdb scripts double-start the
    /// server and write against a not-yet-initiated member).
    MongoDBImage& with_customizer(std::function<void(GenericImage&)> customize) {
        customizers_.push_back(std::move(customize));
        return *this;
    }

    // --- Getters ---

    const std::string& replica_set_name() const noexcept { return replica_set_name_; }
    const std::string& database() const noexcept { return database_; }

    /// Render the full configuration — the replica-set command line, both
    /// readiness waits, and the post-start hook that initiates the set and
    /// waits for PRIMARY — into a plain GenericImage: the drop-down escape
    /// hatch when you need a raw core `Container` instead of a MongoDBContainer.
    /// Extend the result, don't rebuild it: replacing the command or the
    /// hooks disables the replica-set choreography. Throws Error on an
    /// invalid config (a replica-set name outside [A-Za-z0-9_-];
    /// MONGO_INITDB_ROOT_* env set through with_env) before any daemon
    /// contact.
    GenericImage to_generic() const;

    /// Create and start the container, initiate the replica set, and wait
    /// until the node is the writable PRIMARY. Returns the running handle;
    /// throws — and removes the partial container — if any phase fails
    /// (`StartupTimeoutError` when readiness or the election never arrives,
    /// DockerError for daemon failures), like `GenericImage::start()`.
    MongoDBContainer start() const;

private:
    GenericImage image_;                  ///< pin + port + pass-through state
    std::string replica_set_name_{"rs0"}; ///< rendered into cmd + the initiate JS
    std::string database_{"test"};        ///< the no-arg connection_string target
    std::vector<std::function<void(GenericImage&)>> customizers_; ///< applied last
};

/// A running MongoDB server (a single-node replica-set PRIMARY): connection
/// getters plus the owned container.
///
/// Move-only — it owns the core `Container`, whose destructor force-removes the
/// server (RAII teardown; `container().keep()` or reuse opt out, see
/// Container).
class MongoDBContainer {
public:
    /// The address a client in the test process connects to. Resolved once,
    /// when the container started.
    const std::string& host() const noexcept { return host_; }

    /// The host port published for the server port 27017. Resolved once,
    /// when the container started.
    std::uint16_t port() const noexcept { return port_; }

    /// A ready-to-use MongoDB URI for the configured default database:
    /// `mongodb://<host>:<port>/<database>?directConnection=true`. Feed it
    /// verbatim to mongocxx (or any driver / CLI tool).
    ///
    /// `directConnection=true` makes every driver talk to this server
    /// directly instead of discovering the replica set — necessary because
    /// the set's advertised member address is container-internal and not
    /// reachable through the port mapping. Transactions and change streams
    /// work over a direct connection; do not remove the parameter, and do
    /// not add `replicaSet=` (it would switch drivers back into discovery).
    std::string connection_string() const { return connection_string(database_); }

    /// Same URI pointing at `database` instead of the configured default.
    /// An empty name yields `mongodb://<host>:<port>/?directConnection=true`
    /// (the driver default applies; the '/' stays — strict URI parsers
    /// reject options without it).
    std::string connection_string(const std::string& database) const;

    /// The replica-set name the server runs under (for `rs.*` assertions or
    /// hand-built in-network URIs).
    const std::string& replica_set_name() const noexcept { return replica_set_name_; }

    /// The configured default database name.
    const std::string& database() const noexcept { return database_; }

    /// Run a JavaScript snippet inside the container via
    /// `mongosh --quiet --eval` against the local server and return its
    /// output and exit code — seeding and assertions without a C++ driver:
    /// `mongo.mongosh("db.orders.countDocuments({})").stdout_data == "0\n"`.
    /// The snippet starts in the configured default database.
    ExecResult mongosh(const std::string& js) const;

    /// The underlying container handle: exec, read logs, copy files,
    /// `stop()` early, `keep()` it past the test.
    Container& container() noexcept { return container_; }
    const Container& container() const noexcept { return container_; }

private:
    friend class MongoDBImage;
    MongoDBContainer(Container container, std::string replica_set_name, std::string database)
        : container_(std::move(container)), replica_set_name_(std::move(replica_set_name)),
          database_(std::move(database)), host_(container_.host()),
          port_(container_.get_host_port(tcp(MongoDBImage::kPort))) {}

    Container container_;
    std::string replica_set_name_;
    std::string database_;
    std::string host_;       ///< resolved once at start()
    std::uint16_t port_ = 0; ///< resolved once at start()
};

} // namespace testcontainers::modules
