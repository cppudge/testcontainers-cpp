#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testcontainers/Container.hpp"
#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"

namespace testcontainers::modules {

class ScyllaDBContainer;

/// A reusable, copyable description of a single-node ScyllaDB server for
/// tests: CQL on port 9042, developer mode, one shard, no authentication —
/// the vendor-recommended CI shape, typically CQL-ready in seconds. `start()`
/// waits until the node answers CQL and returns a `ScyllaDBContainer` whose
/// getters feed any Cassandra-compatible driver; no client library is
/// required or linked.
///
/// The `with_*` builders mutate in place and return `*this`, so a named config
/// can be configured incrementally and started many times. Core options the
/// module does not surface are reached through `with_customizer`;
/// `to_generic()` drops down to a plain `GenericImage` entirely.
class ScyllaDBImage {
public:
    /// The pinned default image — the current LTS line (the tag floats over
    /// its patch releases). The image is source-available (free tier: 10 TB /
    /// 50 vCPUs per organization — a test container is far inside; CI and
    /// automated-testing use is explicitly permitted for commercial ScyllaDB
    /// customers too). Override with `with_image`; any tag with the same
    /// entrypoint contract works — `scylladb/scylla:6.2` is the last
    /// AGPL-licensed open-source release.
    static constexpr std::string_view kDefaultImage = "scylladb/scylla:2026.1";

    /// The CQL port INSIDE the container: peers on a shared docker network
    /// connect to `<alias-or-name>:kPort`; the test process itself uses
    /// `ScyllaDBContainer::port()` (the mapped host port) instead.
    static constexpr std::uint16_t kPort = 9042;

    /// A config ready to `start()`: pinned image, port 9042 exposed, managed
    /// flags `--developer-mode=1 --overprovisioned=1 --smp 1 --memory 512M`,
    /// readiness = the CQL-listening log line followed by an in-container
    /// cqlsh query, and a 120s startup budget (a first boot initializes the
    /// data directory).
    ScyllaDBImage();

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Override the pinned image with a full reference "name[:tag]" (tag
    /// defaults to "latest").
    ScyllaDBImage& with_image(const std::string& reference);

    /// Number of CPU shards (`--smp`). Default 1 — the CI-friendly shape;
    /// budget about 512M of `with_memory` per shard when raising it. Values
    /// below 1 throw at `start()`, before any daemon contact.
    ScyllaDBImage& with_smp(int shards);

    /// Total server memory (`--memory`), in Seastar's size format — "512M",
    /// "2G". Default "512M", fine for one shard. Empty throws at `start()`.
    ScyllaDBImage& with_memory(std::string amount);

    /// Datacenter name the node reports (`--dc`; the flag also switches the
    /// node's snitch so the name takes effect). Default: unset — the node
    /// reports "datacenter1". Useful when testing DC-aware load balancing;
    /// the started handle's `datacenter()` getter mirrors it. Empty throws
    /// at `start()`.
    ScyllaDBImage& with_datacenter(std::string name);

    /// Append entrypoint arguments (e.g. {"--rack", "r1"}), placed AFTER the
    /// managed flags; the entrypoint keeps the LAST occurrence of a flag, so
    /// your value wins on a duplicate. Two flags belong elsewhere: `--dc` to
    /// `with_datacenter` (a raw duplicate desyncs the `datacenter()` getter),
    /// and `--authenticator` breaks the module's authless readiness probe —
    /// pair it with `with_wait`.
    ScyllaDBImage& with_command_args(std::vector<std::string> args);
    /// Single-argument twin of `with_command_args` (same placement rules).
    ScyllaDBImage& with_command_arg(std::string arg);

    /// Queue a .cql script the module runs through the in-container cqlsh
    /// AFTER the node is ready (ScyllaDB has no initdb.d), in REGISTRATION
    /// order. Only the .cql extension is accepted — anything else throws
    /// here. A failing script fails `start()`; each statement is bounded by
    /// cqlsh's own request timeout. The file is read when start() copies it.
    ///
    /// A reused (adopted) container does NOT re-run scripts — its data is
    /// already seeded; an edited script changes the reuse hash, so the next
    /// `start()` builds a fresh container.
    ScyllaDBImage& with_init_script(std::filesystem::path host_path);

    /// In-memory variant: queue `content` under the given file name (a bare
    /// .cql name, no directories; same ordering as above).
    ScyllaDBImage& with_init_script(const std::string& name, std::string content);

    // --- Pass-throughs to the underlying builder ---

    /// Set an extra container environment variable (TZ and tooling —
    /// ScyllaDB's docker configuration is flag-based, so server options go
    /// through the typed setters and `with_command_args`).
    ScyllaDBImage& with_env(std::string key, std::string value);

    ScyllaDBImage& with_label(std::string key, std::string value);

    /// Join a user-defined network; peers resolve this container by
    /// name/alias at `<alias>:9042` (kPort, not the mapped host port).
    ScyllaDBImage& with_network(std::string network);
    ScyllaDBImage& with_network(const Network& network);
    ScyllaDBImage& with_network_alias(std::string alias);

    /// Enable container reuse (effective only when reuse is also enabled
    /// globally — see GenericImage::with_reuse). An adopted node skips the
    /// boot and keeps its data; init scripts are not re-run.
    ScyllaDBImage& with_reuse(bool reuse = true);

    /// REPLACE the default readiness pair — the CQL-listening log line, then
    /// an in-container cqlsh query — with a custom strategy (the first call
    /// drops the module's probes; repeatable — further waits run in order
    /// under the same timeout).
    ScyllaDBImage& with_wait(WaitFor wait);

    /// Budget for the readiness phase (default: 120s — a first boot
    /// initializes the data directory and routinely takes tens of seconds on
    /// loaded CI). Init scripts run after readiness through the exec
    /// transport and are not under this budget; image pull time does not
    /// count either.
    ScyllaDBImage& with_startup_timeout(std::chrono::milliseconds timeout);

    /// Retry the whole create→start→wait sequence up to `n` times.
    ScyllaDBImage& with_startup_attempts(int n);

    /// Register a callback that customizes the underlying `GenericImage` —
    /// the channel for options this module does not surface (mounts, extra
    /// ports such as the shard-aware 19042, pull policy, ...). Customizers
    /// run when the config is rendered (`start()` / `to_generic()`), in
    /// registration order, AFTER the module's own rendering — what they set
    /// wins. A wait added here runs IN ADDITION to the default probe (unlike
    /// `with_wait`); a wholesale `with_cmd` drops the managed flags — prefer
    /// `with_command_args`.
    ScyllaDBImage& with_customizer(std::function<void(GenericImage&)> customize);

    // --- Getters ---

    int smp() const noexcept { return smp_; }
    const std::string& memory() const noexcept { return memory_; }
    /// The datacenter the started node will report ("datacenter1" unless
    /// `with_datacenter` was called).
    const std::string& datacenter() const noexcept { return datacenter_; }

    /// Render the full configuration — managed flags, init-script copies and
    /// their post-ready hook, readiness pair, customizers — into a plain
    /// GenericImage: the drop-down escape hatch when you need a raw core
    /// `Container` instead of a ScyllaDBContainer. Throws Error on an
    /// invalid config (smp below 1, empty memory or datacenter) before any
    /// daemon contact.
    GenericImage to_generic() const;

    /// Create, start, wait until the node answers CQL, then run any init
    /// scripts. Throws Error on config errors before touching the daemon;
    /// DockerError / StartupTimeoutError from the run itself, like
    /// `GenericImage::start()`.
    ScyllaDBContainer start() const;

private:
    GenericImage image_;                        ///< pin + port + pass-through state
    int smp_{1};                                ///< --smp
    std::string memory_{"512M"};                ///< --memory
    std::string datacenter_{"datacenter1"};     ///< reported name; rendered only when set
    bool datacenter_set_{false};                ///< with_datacenter called
    std::vector<std::string> extra_args_;       ///< after managed flags (last wins)
    std::vector<CopyToContainer> init_scripts_; ///< ordered /tmp copies, run by the hook
    std::vector<WaitFor> waits_;                ///< empty = the default log + cqlsh pair
    std::vector<std::function<void(GenericImage&)>> customizers_; ///< applied last
};

/// A running single-node ScyllaDB server: contact-point getters plus the
/// owned container.
///
/// Move-only — it owns the core `Container`, whose destructor force-removes
/// the server (RAII teardown; `container().keep()` or reuse opt out, see
/// Container).
class ScyllaDBContainer {
public:
    /// The address a client in the test process connects to. Resolved once,
    /// when the container started (as are all getters here).
    const std::string& host() const noexcept { return host_; }

    /// The host port published for CQL port 9042. A container restarted by
    /// hand gets fresh ephemeral ports — re-resolve via `container()` then.
    std::uint16_t port() const noexcept { return port_; }

    /// `"<host>:<port>"`, e.g. "localhost:32773", for clients that take
    /// host:port contact nodes. The cassandra.h-style APIs take the pieces
    /// instead: `cass_cluster_set_contact_points(c, host().c_str())` +
    /// `cass_cluster_set_port(c, port())`.
    const std::string& contact_point() const noexcept { return contact_point_; }

    /// The datacenter this node reports, e.g. "datacenter1" — what DC-aware
    /// load balancing wants: `cass_cluster_set_load_balance_dc_aware(c,
    /// datacenter().c_str(), 0, cass_false)`.
    const std::string& datacenter() const noexcept { return datacenter_; }

    /// Run CQL through the in-container cqlsh against the node's own
    /// address — zero-dependency seeding and asserts; ';'-separated
    /// statements are fine. Output is cqlsh's aligned table — assert with
    /// substrings (e.g. "(1 rows)"), not exact matches. A failing statement
    /// is reported via the result's exit_code/stderr_data, not thrown.
    ExecResult exec_cql(const std::string& cql) const;

    /// The underlying container handle: exec in-image tools (`nodetool
    /// status`, ...), read logs, copy files, `stop()` early, `keep()` it
    /// past the test.
    Container& container() noexcept { return container_; }
    const Container& container() const noexcept { return container_; }

private:
    friend class ScyllaDBImage;
    ScyllaDBContainer(Container container, std::string datacenter)
        : container_(std::move(container)), datacenter_(std::move(datacenter)),
          host_(container_.host()), port_(container_.get_host_port(tcp(ScyllaDBImage::kPort))),
          contact_point_(host_ + ":" + std::to_string(port_)) {}

    Container container_;
    std::string datacenter_;
    std::string host_;          ///< resolved once at start()
    std::uint16_t port_ = 0;    ///< resolved once at start()
    std::string contact_point_; ///< "<host>:<port>", assembled once
};

} // namespace testcontainers::modules
