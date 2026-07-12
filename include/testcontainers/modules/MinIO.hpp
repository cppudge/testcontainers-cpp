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

class MinIOContainer;

/// A reusable, copyable description of a MinIO object-storage server — an
/// S3-compatible endpoint for tests: pinned image, S3 + console ports, root
/// credentials, buckets to pre-create, and an HTTP readiness probe. `start()`
/// runs it and returns a `MinIOContainer` whose getters hand an S3 client
/// everything it needs (endpoint URL, access/secret key); no client SDK is
/// required or linked.
///
/// The `with_*` builders mutate in place and return `*this`, so a named config
/// can be configured incrementally and started many times. Core options the
/// module does not surface are reached through `with_customizer`;
/// `to_generic()` drops down to a plain `GenericImage` entirely.
class MinIOImage {
public:
    /// The pinned default image — the final community release published to
    /// Docker Hub (upstream stopped publishing images in late 2025, so the
    /// tag is frozen; a `-cpuv1` twin exists for old x86-64 CPUs). Override
    /// with `with_image`; any image that keeps the MinIO contract
    /// (MINIO_ROOT_* env, the `server` command, `mc` on PATH for
    /// `with_bucket`) works.
    static constexpr std::string_view kDefaultImage = "minio/minio:RELEASE.2025-09-07T16-13-09Z";

    /// The S3 API port INSIDE the container: peers on a shared docker network
    /// use `http://<alias-or-name>:kS3Port`; the test process itself uses
    /// `MinIOContainer::s3_url()` (the mapped host port) instead.
    static constexpr std::uint16_t kS3Port = 9000;

    /// The web-console port INSIDE the container — fixed by the module's
    /// `--console-address :9001` (the server would otherwise pick a random
    /// port every boot, which could not be exposed).
    static constexpr std::uint16_t kConsolePort = 9001;

    /// A config ready to `start()`: pinned image, ports 9000 + 9001 exposed,
    /// command `server /data --console-address :9001`, credentials
    /// minioadmin/minioadmin, readiness = HTTP 200 from `/minio/health/cluster`.
    MinIOImage();

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Override the pinned image with a full reference "name[:tag]" (tag
    /// defaults to "latest"). The module keeps its command, so the image must
    /// understand `server /data --console-address :9001`.
    MinIOImage& with_image(const std::string& reference);

    /// The root access key (MINIO_ROOT_USER) — what S3 clients send as the
    /// access key ID. Default "minioadmin" (the image default). The server
    /// requires at least 3 characters — shorter throws at `start()`, before
    /// any daemon contact (the server's own failure mode is a fatal exit
    /// followed by the full startup timeout). Test credentials only: the
    /// value is visible via inspect.
    MinIOImage& with_access_key(std::string access_key);

    /// The root secret key (MINIO_ROOT_PASSWORD). Default "minioadmin". The
    /// server requires at least 8 characters — shorter throws at `start()`.
    MinIOImage& with_secret_key(std::string secret_key);

    /// Create a bucket once the server is ready, via the in-image `mc`
    /// client (`mc mb --ignore-existing`, so a pre-seeded store — say, a
    /// custom image with data baked in — is fine). Repeatable; an empty name
    /// throws at `start()`; a failed creation fails `start()`. The hook
    /// first points the container's own `mc` config at the server under the
    /// alias `tc` — execs against `container()` can use paths like
    /// `tc/<bucket>` afterwards. The bucket list also renders into a label,
    /// so container reuse sees it: a changed list builds a fresh container
    /// (hooks do not re-run on an adopted one).
    MinIOImage& with_bucket(std::string name);

    // --- Pass-throughs to the underlying builder ---

    /// Set an extra environment variable — the server's MINIO_* knobs
    /// (MINIO_REGION, MINIO_BROWSER=off, MINIO_DOMAIN, ...). The credential
    /// pair belongs to with_access_key/with_secret_key: the module appends
    /// those env keys last, so they win over raw duplicates set here. The
    /// legacy MINIO_ACCESS_KEY / MINIO_SECRET_KEY spellings are ignored by
    /// the pinned server — use the typed setters.
    MinIOImage& with_env(std::string key, std::string value);

    /// Attach a metadata label. The module's reuse-visibility label
    /// (`org.testcontainers.minio.buckets`) is applied after these, so it
    /// wins on a duplicate key.
    MinIOImage& with_label(std::string key, std::string value);

    /// Join a user-defined network; peers resolve this container by
    /// name/alias at `http://<alias>:9000` (kS3Port, not the mapped host
    /// port), with the same credentials. Mind that presigned URLs embed the
    /// endpoint they were generated against — generate them against the
    /// address their consumer will dial.
    MinIOImage& with_network(std::string network);
    MinIOImage& with_network(const Network& network);
    MinIOImage& with_network_alias(std::string alias);

    /// Enable container reuse (effective only when reuse is also enabled
    /// globally — see GenericImage::with_reuse). An adopted server keeps its
    /// object store; the `with_bucket` list participates in the reuse match
    /// via the label, so a changed list builds a fresh container.
    MinIOImage& with_reuse(bool reuse = true);

    /// REPLACE the default readiness probe with a custom strategy (the first
    /// call drops the module's `/minio/health/cluster` probe; repeatable —
    /// further waits run in order under the same timeout). The escape for
    /// TLS setups: the default probe speaks plain HTTP only.
    MinIOImage& with_wait(WaitFor wait);

    /// Budget for the readiness phase (default: 60s; observed cold boots are
    /// a few seconds). Image pull time does not count against it.
    MinIOImage& with_startup_timeout(std::chrono::milliseconds timeout);

    /// Retry the whole create→start→wait sequence up to `n` times.
    MinIOImage& with_startup_attempts(int n);

    /// Register a callback that customizes the underlying `GenericImage` —
    /// the channel for options this module does not surface (mounts, pull
    /// policy, extra `server` flags via with_cmd, ...). Customizers run when
    /// the config is rendered (`start()` / `to_generic()`), in registration
    /// order, AFTER the module's own rendering — what they set wins. A wait
    /// added here runs IN ADDITION to the default probe (unlike `with_wait`,
    /// which replaces it). Two caveats: replacing the command drops the fixed
    /// console address (`console_url()` then points at nothing), and setting
    /// the MINIO_ROOT_* env here desyncs the credential getters and the
    /// bucket hook — use the typed setters instead.
    MinIOImage& with_customizer(std::function<void(GenericImage&)> customize);

    // --- Getters ---

    const std::string& access_key() const noexcept { return access_key_; }
    const std::string& secret_key() const noexcept { return secret_key_; }
    /// Buckets queued for creation, in registration order.
    const std::vector<std::string>& buckets() const noexcept { return buckets_; }

    /// Render the full configuration — command, credential env, readiness
    /// probe, bucket hook + label, customizers — into a plain GenericImage:
    /// the drop-down escape hatch when you need a raw core `Container`
    /// instead of a MinIOContainer. Throws Error on an invalid config
    /// (access key shorter than 3 characters, secret key shorter than 8,
    /// an empty bucket name) before any daemon contact.
    GenericImage to_generic() const;

    /// Create, start, wait until the S3 API reports itself writable, then
    /// create any `with_bucket` buckets. Throws Error on config errors
    /// before touching the daemon; DockerError / StartupTimeoutError from
    /// the run itself, like `GenericImage::start()`.
    MinIOContainer start() const;

private:
    GenericImage image_;                   ///< pin + ports + pass-through state
    std::string access_key_{"minioadmin"}; ///< MINIO_ROOT_USER, rendered last
    std::string secret_key_{"minioadmin"}; ///< MINIO_ROOT_PASSWORD, rendered last
    std::vector<std::string> buckets_;     ///< hook-created; also a reuse label
    std::vector<WaitFor> waits_;           ///< empty = the default health probe
    std::vector<std::function<void(GenericImage&)>> customizers_; ///< applied last
};

/// A running MinIO server: S3 endpoint getters plus the owned container.
///
/// Move-only — it owns the core `Container`, whose destructor force-removes
/// the server including the image's anonymous `/data` volume (RAII teardown;
/// `container().keep()` or reuse opt out, see Container).
class MinIOContainer {
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

    /// The root access key — what an S3 client sends as the access key ID
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

    /// The web-console URL, e.g. `http://localhost:32769` — log in with
    /// access_key()/secret_key() (pairs well with `container().keep()` after
    /// a failed test). At the pinned release the community console is an
    /// object BROWSER only (administration moved to `mc`); with
    /// MINIO_BROWSER=off nothing listens here.
    std::string console_url() const;

    /// The underlying container handle: exec the in-image `mc` (after a
    /// `with_bucket` start it is already aliased to the server as `tc`),
    /// read logs, copy files, `stop()` early, `keep()` it past the test.
    Container& container() noexcept { return container_; }
    const Container& container() const noexcept { return container_; }

private:
    friend class MinIOImage;
    MinIOContainer(Container container, std::string access_key, std::string secret_key)
        : container_(std::move(container)), access_key_(std::move(access_key)),
          secret_key_(std::move(secret_key)), host_(container_.host()),
          s3_port_(container_.get_host_port(tcp(MinIOImage::kS3Port))),
          console_port_(container_.get_host_port(tcp(MinIOImage::kConsolePort))) {}

    Container container_;
    std::string access_key_;
    std::string secret_key_;
    std::string host_;               ///< resolved once at start()
    std::uint16_t s3_port_ = 0;      ///< resolved once at start()
    std::uint16_t console_port_ = 0; ///< resolved once at start()
};

} // namespace testcontainers::modules
