#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "testcontainers/Container.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"

namespace testcontainers::modules {

class OpenSearchContainer;

/// A reusable, copyable description of a single-node OpenSearch server for
/// tests, running with the security plugin DISABLED: plain HTTP on one
/// published port, no credentials — `OpenSearchContainer::http_url()` is all
/// a REST client needs. Throwaway test data only; no client library is
/// required or linked.
///
/// The `with_*` builders mutate in place and return `*this`, so a named config
/// can be configured incrementally and started many times. Core options the
/// module does not surface are reached through `with_customizer`;
/// `to_generic()` drops down to a plain `GenericImage` entirely.
class OpenSearchImage {
public:
    /// The pinned default image (the newest stable of the active 3.x line;
    /// the hub publishes no minor-line tags, so the pin is patch-level).
    /// Override with `with_image`; any `opensearchproject/opensearch` tag
    /// from 2.12 up keeps the env contract this module relies on.
    static constexpr std::string_view kDefaultImage = "opensearchproject/opensearch:3.7.0";

    /// The REST port INSIDE the container. Peers on a shared docker network
    /// connect to `http://<alias-or-name>:kPort`; the test process itself
    /// uses `OpenSearchContainer::http_url()` (the mapped host port).
    static constexpr std::uint16_t kPort = 9200;

    /// A config ready to `start()`: pinned image, port 9200 exposed, security
    /// plugin disabled, single-node discovery, 512 MB heap, readiness =
    /// HTTP 200 from `/_cluster/health` through the published port, and a
    /// 120s startup budget (a JVM engine booting a gigabyte of plugins).
    OpenSearchImage();

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Override the pinned image with a full reference "name[:tag]" (tag
    /// defaults to "latest"), e.g. another 3.x patch or a registry mirror.
    /// Images older than 2.12 are untested.
    OpenSearchImage& with_image(const std::string& reference);

    /// Set an environment variable. Applied after the module's own settings,
    /// so on a duplicate key YOURS wins (the managed keys are engine tuning,
    /// not credential mirrors — no getter can desync). Keys shaped like
    /// `section.setting` become OpenSearch settings —
    /// `with_env("cluster.name", "it")` configures the server. Overriding
    /// the heap (OPENSEARCH_JAVA_OPTS) or `discovery.type` is fair game;
    /// re-enabling security (DISABLE_SECURITY_PLUGIN /
    /// DISABLE_INSTALL_DEMO_CONFIG) switches 9200 to https with demo certs,
    /// which the module's plain-HTTP readiness probe can never pass —
    /// replace it via `with_wait` then (an in-container
    /// `curl -sk --fail -u admin:<password> https://localhost:9200/` through
    /// `wait_for::successful_shell_command` is the working recipe).
    OpenSearchImage& with_env(std::string key, std::string value);

    OpenSearchImage& with_label(std::string key, std::string value);

    /// Join a user-defined network; peers resolve this container by
    /// name/alias at `http://<alias>:9200` (kPort, not the mapped host port).
    OpenSearchImage& with_network(std::string network);
    OpenSearchImage& with_network(const Network& network);
    OpenSearchImage& with_network_alias(std::string alias);

    /// Enable container reuse (effective only when reuse is also enabled
    /// globally — see GenericImage::with_reuse). An adopted server keeps its
    /// indices: same config, next run, data intact.
    OpenSearchImage& with_reuse(bool reuse = true);

    /// REPLACE the default readiness probe with a custom strategy (the first
    /// call drops the module's `/_cluster/health` probe; repeatable —
    /// further waits run in order under the same timeout). Required when a
    /// security-enabled setup turns 9200 into https (see `with_env`).
    OpenSearchImage& with_wait(WaitFor wait);

    /// Budget for the readiness phase (default: 120s — a JVM engine booting
    /// ~1 GB of plugins needs it on slow CI; observed warm boots are
    /// 10-20s). Image pull time does not count against it.
    OpenSearchImage& with_startup_timeout(std::chrono::milliseconds timeout);

    /// Retry the whole create→start→wait sequence up to `n` times.
    OpenSearchImage& with_startup_attempts(int n);

    /// Register a callback that customizes the underlying `GenericImage` —
    /// the channel for options this module does not surface (mounts, memory
    /// limits, extra ports such as the performance analyzer's 9600, pull
    /// policy, ...). Customizers run when the config is rendered (`start()`
    /// / `to_generic()`), in registration order, AFTER the module's own
    /// rendering — what they set wins. A wait added here runs IN ADDITION
    /// to the default probe (unlike `with_wait`, which replaces it).
    OpenSearchImage& with_customizer(std::function<void(GenericImage&)> customize);

    // --- Getters ---

    /// Render the full configuration — env, port, health probe, customizers
    /// — into a plain GenericImage: the drop-down escape hatch when you need
    /// a raw core `Container` instead of an OpenSearchContainer.
    GenericImage to_generic() const;

    /// Create, start, and wait until `/_cluster/health` answers 200 through
    /// the published port. Throws DockerError / StartupTimeoutError from the
    /// run, like `GenericImage::start()`.
    OpenSearchContainer start() const;

private:
    GenericImage image_;         ///< pin + port + env (managed keys first) + pass-throughs
    std::vector<WaitFor> waits_; ///< empty = the default /_cluster/health probe
    std::vector<std::function<void(GenericImage&)>> customizers_; ///< applied last
};

/// A running OpenSearch server: URL getters plus the owned container.
///
/// Move-only — it owns the core `Container`, whose destructor force-removes
/// the server (RAII teardown; `container().keep()` or reuse opt out, see
/// Container).
class OpenSearchContainer {
public:
    /// The address a client in the test process connects to. Resolved once,
    /// when the container started.
    const std::string& host() const noexcept { return host_; }

    /// The host port published for the REST port 9200. Resolved once, when
    /// the container started; a container restarted by hand gets fresh
    /// ephemeral ports — re-resolve via `container()` then.
    std::uint16_t port() const noexcept { return port_; }

    /// The REST base URL, e.g. `http://localhost:32768` — no trailing slash,
    /// no credentials (security is disabled). Point any HTTP client or the
    /// opensearch-cpp client at it; endpoints live under `/<index>/_doc`,
    /// `/_search`, `/_cluster/health`, ...
    std::string http_url() const;

    /// The underlying container handle: exec the in-image `curl` for
    /// driver-free seeding and asserts, read logs, copy files, `stop()`
    /// early, `keep()` it past the test.
    Container& container() noexcept { return container_; }
    const Container& container() const noexcept { return container_; }

private:
    friend class OpenSearchImage;
    explicit OpenSearchContainer(Container container)
        : container_(std::move(container)), host_(container_.host()),
          port_(container_.get_host_port(tcp(OpenSearchImage::kPort))) {}

    Container container_;
    std::string host_;       ///< resolved once at start()
    std::uint16_t port_ = 0; ///< resolved once at start()
};

} // namespace testcontainers::modules
