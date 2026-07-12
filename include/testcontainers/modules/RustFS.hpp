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

class RustFSContainer;

/// A reusable, copyable description of a RustFS server — an S3-compatible
/// object store — for tests: pinned image, S3 + console ports, managed
/// access/secret credentials, and an HTTP readiness probe. `start()` runs it
/// and returns a `RustFSContainer` whose getters hand an S3 client everything
/// it needs; no client SDK is required or linked.
///
/// The `with_*` builders mutate in place and return `*this`, so a named config
/// can be configured incrementally and started many times. Core options the
/// module does not surface are reached through `with_customizer`;
/// `to_generic()` drops down to a plain `GenericImage` entirely.
class RustFSImage {
public:
    /// The pinned default image. RustFS has no stable release line yet, so
    /// this is the newest upstream beta — expect the pin to move with
    /// upstream until 1.0.0 lands. Override with `with_image`: another
    /// release tag, a `-glibc` variant, or a mirror; the image must keep the
    /// official contract (RUSTFS_* env, `/health` on the S3 port).
    static constexpr std::string_view kDefaultImage = "rustfs/rustfs:1.0.0-beta.8";

    /// The S3 API port INSIDE the container: peers on a shared docker network
    /// use `http://<alias-or-name>:kS3Port`; the test process itself uses
    /// `RustFSContainer::s3_url()` (the mapped host port) instead.
    static constexpr std::uint16_t kS3Port = 9000;

    /// The embedded web-console port INSIDE the container (the console is
    /// enabled by default; the UI lives under `/rustfs/console/`).
    static constexpr std::uint16_t kConsolePort = 9001;

    /// A config ready to `start()`: pinned image, ports 9000 + 9001 exposed,
    /// credentials rustfsadmin/rustfsadmin, readiness = HTTP 200 from
    /// `/health` on the S3 port. The image boots commandless — its entrypoint
    /// stores objects in the image-declared `/data` volume.
    RustFSImage();

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Override the pinned image with a full reference "name[:tag]" (tag
    /// defaults to "latest").
    RustFSImage& with_image(const std::string& reference);

    /// The S3 access key (RUSTFS_ACCESS_KEY) — what S3 clients send as the
    /// access key ID. Default "rustfsadmin" (the image default). Empty throws
    /// at `start()`, before any daemon contact; the server itself imposes no
    /// documented length rules. Test credentials only: visible via inspect.
    RustFSImage& with_access_key(std::string access_key);

    /// The matching secret key (RUSTFS_SECRET_KEY). Default "rustfsadmin".
    /// Empty throws at `start()`.
    RustFSImage& with_secret_key(std::string secret_key);

    // --- Pass-throughs to the underlying builder ---

    /// Set an extra environment variable — the image's own knobs
    /// (RUSTFS_CONSOLE_ENABLE, RUSTFS_OBS_LOGGER_LEVEL, ...). The credential
    /// pair belongs to with_access_key/with_secret_key: the module appends
    /// those env keys last, so they win over raw duplicates set here.
    RustFSImage& with_env(std::string key, std::string value);

    RustFSImage& with_label(std::string key, std::string value);

    /// Join a user-defined network; peers resolve this container by
    /// name/alias at `http://<alias>:9000` (kS3Port, not the mapped host
    /// port), with the same credentials and path-style bucket addressing.
    RustFSImage& with_network(std::string network);
    RustFSImage& with_network(const Network& network);
    RustFSImage& with_network_alias(std::string alias);

    /// Enable container reuse (effective only when reuse is also enabled
    /// globally — see GenericImage::with_reuse). An adopted server keeps the
    /// buckets and objects in its `/data` volume.
    RustFSImage& with_reuse(bool reuse = true);

    /// REPLACE the default readiness probe with a custom strategy (the first
    /// call drops the module's `/health` probe; repeatable — further waits
    /// run in order under the same timeout). The escape for TLS setups: the
    /// default probe speaks plain HTTP only.
    RustFSImage& with_wait(WaitFor wait);

    /// Budget for the readiness phase (default: 60s; cold boots take a few
    /// seconds). Image pull time does not count against it.
    RustFSImage& with_startup_timeout(std::chrono::milliseconds timeout);

    /// Retry the whole create→start→wait sequence up to `n` times.
    RustFSImage& with_startup_attempts(int n);

    /// Register a callback that customizes the underlying `GenericImage` —
    /// the channel for options this module does not surface (mounts, pull
    /// policy, extra `rustfs` flags via with_cmd, ...). Customizers run when
    /// the config is rendered (`start()` / `to_generic()`), in registration
    /// order, AFTER the module's own rendering — what they set wins. A wait
    /// added here runs IN ADDITION to the default probe (unlike `with_wait`,
    /// which replaces it). Do not set the RUSTFS_* credential env here — it
    /// would desync the credential getters; use the typed setters instead.
    RustFSImage& with_customizer(std::function<void(GenericImage&)> customize);

    // --- Getters ---

    const std::string& access_key() const noexcept { return access_key_; }
    const std::string& secret_key() const noexcept { return secret_key_; }

    /// Render the full configuration — credential env, readiness probe,
    /// customizers — into a plain GenericImage: the drop-down escape hatch
    /// when you need a raw core `Container` instead of a RustFSContainer.
    /// Throws Error on an invalid config (an empty access or secret key)
    /// before any daemon contact.
    GenericImage to_generic() const;

    /// Create, start, and wait until `/health` answers 200 on the S3 port.
    /// Throws Error on config errors before touching the daemon; DockerError
    /// / StartupTimeoutError from the run itself, like
    /// `GenericImage::start()`.
    RustFSContainer start() const;

private:
    GenericImage image_;                    ///< pin + ports + pass-through state
    std::string access_key_{"rustfsadmin"}; ///< RUSTFS_ACCESS_KEY, rendered last
    std::string secret_key_{"rustfsadmin"}; ///< RUSTFS_SECRET_KEY, rendered last
    std::vector<WaitFor> waits_;            ///< empty = the default /health probe
    std::vector<std::function<void(GenericImage&)>> customizers_; ///< applied last
};

/// A running RustFS server: S3 endpoint getters plus the owned container.
///
/// Move-only — it owns the core `Container`, whose destructor force-removes
/// the server including the image's anonymous `/data` volume (RAII teardown;
/// `container().keep()` or reuse opt out, see Container).
class RustFSContainer {
public:
    /// The address a client in the test process connects to. Resolved once,
    /// when the container started.
    const std::string& host() const noexcept { return host_; }

    /// The host port published for the S3 API port 9000. Resolved once, when
    /// the container started; a container restarted by hand gets fresh
    /// ephemeral ports — re-resolve via `container()` then.
    std::uint16_t s3_port() const noexcept { return s3_port_; }

    /// The host port published for the web console (container port 9001).
    std::uint16_t console_port() const noexcept { return console_port_; }

    /// The S3 access key — what an S3 client sends as the access key ID
    /// (AWS_ACCESS_KEY_ID).
    const std::string& access_key() const noexcept { return access_key_; }
    /// The matching secret key (AWS_SECRET_ACCESS_KEY).
    const std::string& secret_key() const noexcept { return secret_key_; }

    /// The S3 endpoint for clients in THIS process, e.g.
    /// `http://localhost:32768` — plain HTTP, no trailing slash. Use it as an
    /// endpoint override with PATH-STYLE addressing (the bucket rides in the
    /// URL path, not the hostname — virtual-host style needs wildcard DNS a
    /// test container does not have): AWS SDK for C++ — set
    /// `endpointOverride = s3_url()` on the client config and construct the
    /// S3Client with `useVirtualAddressing = false` (any region works;
    /// "us-east-1" for SDKs that insist on one).
    std::string s3_url() const;

    /// The web-console URL, e.g. `http://localhost:32769/rustfs/console/` —
    /// the UI lives under that path (the console port answers 403 at `/`);
    /// log in with access_key()/secret_key() (pairs well with
    /// `container().keep()` after a failed test). If the console was
    /// disabled via `with_env("RUSTFS_CONSOLE_ENABLE", "false")`, the port
    /// is still published but nothing answers.
    std::string console_url() const;

    /// The underlying container handle: exec in-image tools (the image ships
    /// `curl` and a shell), read logs, copy files, `stop()` early, `keep()`
    /// it past the test.
    Container& container() noexcept { return container_; }
    const Container& container() const noexcept { return container_; }

private:
    friend class RustFSImage;
    RustFSContainer(Container container, std::string access_key, std::string secret_key)
        : container_(std::move(container)), access_key_(std::move(access_key)),
          secret_key_(std::move(secret_key)), host_(container_.host()),
          s3_port_(container_.get_host_port(tcp(RustFSImage::kS3Port))),
          console_port_(container_.get_host_port(tcp(RustFSImage::kConsolePort))) {}

    Container container_;
    std::string access_key_;
    std::string secret_key_;
    std::string host_;               ///< resolved once at start()
    std::uint16_t s3_port_ = 0;      ///< resolved once at start()
    std::uint16_t console_port_ = 0; ///< resolved once at start()
};

} // namespace testcontainers::modules
