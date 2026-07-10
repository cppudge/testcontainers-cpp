#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/ExecOptions.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/RegistryAuth.hpp"
#include "testcontainers/docker/BuildOptions.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerHost.hpp"
#include "testcontainers/docker/Logs.hpp"
#include "testcontainers/docker/Timeouts.hpp"

namespace testcontainers {

namespace docker {
class ITransport;

/// Receives successive blocks of a streamed body (up to ~64 KiB each). Used
/// on both directions: an upload producer writes blocks into one, a download
/// call delivers response blocks through one. An exception thrown from the
/// sink aborts the transfer and propagates to the caller unchanged.
using ByteSink = std::function<void(const char* data, std::size_t size)>;

/// Writes a request body incrementally: called once per request with the sink
/// to push blocks into; returning ends the body. Each block goes onto the
/// wire as one HTTP chunk (the request is sent with
/// `Transfer-Encoding: chunked`, so the total size never needs to be known
/// up front). An exception aborts the request and propagates unchanged.
using BodyProducer = std::function<void(const ByteSink& sink)>;
} // namespace docker

/// An HTTP response from the Docker Engine API.
struct Response {
    int status_code = 0;
    std::string reason;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    /// Case-insensitive header lookup; returns "" if absent.
    std::string header(std::string_view name) const;

    bool ok() const noexcept { return status_code >= 200 && status_code < 300; }
};

/// A synchronous client for the Docker Engine HTTP API.
///
/// By default each request opens a fresh connection to the daemon over the
/// resolved transport (unix socket / Windows named pipe / TCP / TLS) — the
/// correctness-first choice the Rust reference (bollard) also makes. A scoped
/// `Session` opts one instance into keep-alive reuse for consecutive GETs
/// (used internally by the wait-strategy polling loops, where the per-request
/// TCP/TLS handshake actually costs); a process-wide connection pool is
/// deliberately NOT provided (see docs/TODO.md for the analysis).
class DockerClient {
public:
    /// Construct a client for an explicitly resolved host.
    explicit DockerClient(DockerHost host);

    /// While alive, THIS client instance keeps its daemon connection open
    /// across requests and reuses it for consecutive idempotent GET calls —
    /// all other requests keep opening fresh connections, so a
    /// stale-connection retry can never replay a side effect. If a reused
    /// connection turns out dead (the daemon or an intermediary closed it
    /// while idle), the request is transparently retried ONCE on a fresh
    /// connection; a deadline expiry (TransportTimeoutError) is never
    /// retried. The cached connection is closed when the session ends.
    ///
    /// Sessions make the instance stateful: do not share one DockerClient
    /// instance across threads while a session is active (copies made while
    /// a session is active do not inherit it and stay independent). The
    /// client must outlive the Session — the guard holds a reference.
    class Session {
    public:
        [[nodiscard]] explicit Session(DockerClient& client)
            : client_(client), owns_(!client.session_enabled_) {
            client_.session_enabled_ = true;
        }
        ~Session() {
            if (owns_) {
                client_.end_session();
            }
        }
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

    private:
        DockerClient& client_;
        bool owns_; ///< nested sessions: only the outermost one tears down
    };

    /// Bounded-retry policy for `pull_image` (see `set_pull_retry`).
    struct PullRetry {
        /// Total tries for one pull (>= 1; 1 disables retrying).
        int attempts = 3;
        /// Sleep before the first retry, doubled for each retry after that.
        std::chrono::milliseconds first_delay{1000};
    };

    // The session connection is per-instance state: neither copies nor moves
    // transfer it (a moved-into instance would be stuck in reuse mode with no
    // Session guard to end it, and two instances writing into one socket
    // would interleave requests). The source of a move keeps its session
    // state, so its guard still tears it down. The negotiated API version DOES
    // carry over — it belongs to the daemon, not to the connection.
    DockerClient(const DockerClient& other)
        : host_(other.host_), timeouts_(other.timeouts_), pull_retry_(other.pull_retry_),
          max_response_body_(other.max_response_body_), api_prefix_(other.api_prefix_) {}
    DockerClient& operator=(const DockerClient& other) {
        if (this != &other) {
            host_ = other.host_;
            timeouts_ = other.timeouts_;
            pull_retry_ = other.pull_retry_;
            max_response_body_ = other.max_response_body_;
            api_prefix_ = other.api_prefix_;
            session_enabled_ = false;
            session_transport_.reset();
        }
        return *this;
    }
    // The moved-from source stays usable (it keeps its session state), so its
    // api_prefix_ is reset to "not negotiated yet": moving out of the optional
    // alone would leave it engaged with an empty string — silently unpinned
    // instead of renegotiating on its next typed call.
    DockerClient(DockerClient&& other) noexcept
        : host_(std::move(other.host_)), timeouts_(other.timeouts_), pull_retry_(other.pull_retry_),
          max_response_body_(other.max_response_body_), api_prefix_(std::move(other.api_prefix_)) {
        other.api_prefix_.reset();
    }
    DockerClient& operator=(DockerClient&& other) noexcept {
        if (this != &other) {
            host_ = std::move(other.host_);
            timeouts_ = other.timeouts_;
            pull_retry_ = other.pull_retry_;
            max_response_body_ = other.max_response_body_;
            api_prefix_ = std::move(other.api_prefix_);
            other.api_prefix_.reset();
            session_enabled_ = false;
            session_transport_.reset();
        }
        return *this;
    }
    ~DockerClient() = default;

    /// Construct a client using DockerHost::resolve().
    static DockerClient from_environment();

    const DockerHost& host() const noexcept { return host_; }

    /// Transport deadlines applied to every connection this client opens (a
    /// copy of the client carries its timeouts along). Endpoints known to be
    /// long-polling widen the io deadline internally (`stop` waits up to its
    /// grace period, `build` may have long silent steps); the streaming call
    /// sites (`follow_logs`, exec attach reads) disable it regardless — or,
    /// on their deadline-bounded overloads, re-arm it from the remaining
    /// budget instead.
    void set_transport_timeouts(const docker::TransportTimeouts& timeouts) { timeouts_ = timeouts; }
    const docker::TransportTimeouts& transport_timeouts() const noexcept { return timeouts_; }

    /// Retry policy for `pull_image`: an HTTP 5xx reply to
    /// `POST /images/create` is retried up to `attempts` total tries with a
    /// doubling backoff — that status is how the daemon relays transient
    /// registry trouble (an auth-token endpoint blip, a 502 mid-handshake).
    /// Anything else fails on the first try: 4xx are permanent, an error
    /// embedded in the 200 progress stream is how daemons report a
    /// nonexistent image, and a transport deadline expiry
    /// (TransportTimeoutError) is never retried. `attempts` below 1 counts
    /// as 1. A copy of the client carries the policy along.
    void set_pull_retry(const PullRetry& retry) {
        pull_retry_ = retry;
        pull_retry_.attempts = retry.attempts < 1 ? 1 : retry.attempts;
    }
    const PullRetry& pull_retry() const noexcept { return pull_retry_; }

    /// Cap the response body size the BUFFERED request paths are willing to
    /// hold in memory — `request()`, `logs()`, the string
    /// `copy_from_container` — so a runaway reply becomes a DockerError
    /// instead of unbounded allocation. std::nullopt (the default) keeps the
    /// historical no-limit behavior. The paths that manage their own reads
    /// (the sink download, `copy_from_container_to`, `follow_logs`, exec
    /// output, build progress) are not affected by the cap. A copy of the
    /// client carries the cap along.
    void set_max_response_body(std::optional<std::uint64_t> limit) { max_response_body_ = limit; }
    std::optional<std::uint64_t> max_response_body() const noexcept { return max_response_body_; }

    /// Perform an HTTP request against the daemon and return the full response.
    /// `target` is the path, sent VERBATIM (e.g. "/_ping",
    /// "/v1.43/containers/json") — this raw escape hatch does no API-version
    /// pinning; an unversioned path gets the daemon's default (newest) version.
    /// The typed methods below all pin their paths to the negotiated version.
    Response request(std::string_view method, std::string_view target, std::string_view body = {},
                     const std::vector<std::pair<std::string, std::string>>& headers = {});

    /// `GET /_ping` — true if the daemon answers with a 2xx status.
    bool ping();

    /// `GET /version` — the daemon's operating system (the `Os` field, e.g.
    /// "linux" / "windows"). Cached process-wide on first success: the engine
    /// mode (Linux vs Windows containers) does not change mid-process. Throws
    /// DockerError if the daemon is unreachable or the response is malformed.
    std::string server_os();

    /// True when the daemon is running in Windows-containers mode (server_os()
    /// contains "windows", case-insensitive). Used to skip the Linux-only Ryuk
    /// reaper and to route engine-specific tests.
    bool is_windows_engine();

    // --- Image operations ---

    /// `POST /images/create?fromImage=...` — pull an image (blocks until done).
    /// `image` is "name[:tag]" (tag defaults to "latest").
    ///
    /// When `auth` is provided it is sent verbatim as `X-Registry-Auth`;
    /// otherwise credentials are auto-resolved from the Docker config for the
    /// image's registry. A public pull (no credentials found) is unaffected.
    /// Transient daemon/registry 5xx replies are retried per the `pull_retry()`
    /// policy (3 tries with a 1s-then-2s backoff by default — see
    /// `set_pull_retry` for what is and is not retried).
    void pull_image(const std::string& image,
                    const std::optional<RegistryAuth>& auth = std::nullopt);

    /// `GET /images/{reference}/json` — true when `reference` ("name[:tag]" or an
    /// image ID) resolves on the daemon. A purely local check: no registry is
    /// contacted. Throws DockerError on any daemon error other than a 404.
    bool image_exists(const std::string& reference);

    /// `GET /images/{reference}/json` — a structured snapshot of the image
    /// (`reference` is "name[:tag]", an image ID, or a digest). A purely local
    /// lookup: no registry is contacted. Throws DockerError if the image is
    /// absent (404 -> NotFoundError) or the daemon errors.
    ImageInspect inspect_image(const std::string& reference);

    /// `GET /images/{reference}/json` — the RAW response body (the full inspect
    /// JSON), so callers can read any field `ImageInspect` does not model.
    /// Throws DockerError exactly like `inspect_image`.
    std::string inspect_image_raw(const std::string& reference);

    /// `POST /build` — build an image from a tar build context (`context_tar`, an
    /// `application/x-tar` body). Blocks until the build finishes. The build
    /// progress is decoded as it arrives: each output line goes to `consumer`
    /// (when set), so long builds are observable live. Throws DockerError on a
    /// non-200 status or a build error embedded in the streamed output — the
    /// error message carries the tail of the step output for debugging.
    void build_image(const std::string& context_tar, const docker::BuildOptions& options,
                     const docker::BuildLogConsumer& consumer = {});

    /// Streaming `POST /build`: `context` writes the tar build context
    /// incrementally into the sink it is handed, and each block goes out as
    /// one chunk of the request body — the context is never held in memory
    /// whole (GenericBuildableImage::build() streams host files this way).
    /// Response handling (live progress decoding, embedded-error detection,
    /// the widened silent-step deadline) matches the string overload; on a
    /// mid-upload rejection the daemon's error status is preferred over the
    /// raw transport error.
    void build_image(const docker::BodyProducer& context, const docker::BuildOptions& options,
                     const docker::BuildLogConsumer& consumer = {});

    // --- Container lifecycle ---

    /// `POST /containers/create` — returns the new container id. If the image is
    /// missing (404), pulls it (threading `auth` through) and retries once.
    std::string create_container(const CreateContainerSpec& spec,
                                 const std::optional<RegistryAuth>& auth = std::nullopt);

    /// `POST /containers/{id}/start`.
    void start_container(const std::string& id);

    /// `GET /containers/{id}/json` — throws DockerError if the container is gone.
    ContainerInspect inspect_container(const std::string& id);

    /// `GET /containers/{id}/json` — the RAW response body (the full inspect JSON),
    /// so callers can read any field `ContainerInspect` does not model. Throws
    /// DockerError if the container is gone (404) or the daemon returns a non-200.
    std::string inspect_container_raw(const std::string& id);

    /// `GET /containers/json` filtered by label equality. `all` includes stopped
    /// containers. `label_filters` become Docker's filters={"label":["k=v",...]}.
    std::vector<ContainerSummary>
    list_containers(const std::vector<std::pair<std::string, std::string>>& label_filters,
                    bool all = true);

    /// `POST /containers/{id}/stop` (optional grace period in seconds).
    void stop_container(const std::string& id, std::optional<int> timeout_secs = std::nullopt);

    /// `DELETE /containers/{id}` — force-kill and remove anonymous volumes by default.
    void remove_container(const std::string& id, bool force = true, bool remove_volumes = true);

    /// `GET /containers/{id}/logs` — fetch a snapshot of the container's logs and
    /// demultiplex the (non-TTY) stream into separate stdout / stderr text.
    /// Always a snapshot (`follow=0`); use `follow_logs()` to stream.
    ContainerLogs logs(const std::string& id, const LogOptions& opts = {});

    /// `GET /containers/{id}/logs?follow=1` — stream the multiplexed logs, decoding
    /// frames and invoking `consumer` per chunk until the stream ends (container
    /// stops) or `consumer` returns false. Blocking: run on your own thread for
    /// background consumption. Always streams (`follow=1`). Throws DockerError if the
    /// initial response is not 200.
    void follow_logs(const std::string& id, const LogOptions& opts, const LogConsumer& consumer);

    /// Deadline-bounded follow_logs: same streaming, but the wait for the next
    /// chunk is additionally bounded by the absolute `deadline` — when it
    /// passes, the stream is closed and DeadlineExpired reported instead of
    /// blocking until the container stops. Returns why the stream ended. Used
    /// by the log wait strategy; also handy for "collect output for at most N
    /// seconds" consumers. Throws like the unbounded overload (a deadline that
    /// expires while CONNECTING or reading the response header surfaces as the
    /// transport's own TransportTimeoutError, not as DeadlineExpired).
    FollowEnd follow_logs(const std::string& id, const LogOptions& opts,
                          const LogConsumer& consumer,
                          std::chrono::steady_clock::time_point deadline);

    /// Run `cmd` inside the running container and capture its output and exit
    /// code, using default options. Equivalent to the `opts` overload with a
    /// default-constructed `ExecOptions`.
    ExecResult exec(const std::string& id, const std::vector<std::string>& cmd);

    /// Run `cmd` inside the running container with `opts` (env / working dir /
    /// user / privileged / tty / stdin / detach) and capture its output and exit
    /// code. Creates the exec (`POST /containers/{id}/exec`), starts it
    /// (`POST /exec/{exec_id}/start`) and reads the exit code
    /// (`GET /exec/{exec_id}/json`).
    ///
    /// With `opts.detach == true` the command is only STARTED (fire-and-forget,
    /// `docker exec -d`): create + start are two plain round-trips, nothing is
    /// attached, and the returned ExecResult keeps its defaults — empty output
    /// and exit_code 0; the command is still running, so its real status is
    /// unknown. detach + stdin_data throws DockerError before any daemon
    /// interaction (nothing is attached, so there is no stdin to feed).
    ///
    /// With `opts.tty == false` the returned stream is the multiplexed frame
    /// format and is demuxed into `stdout_data` / `stderr_data`. With
    /// `opts.tty == true` the stream is raw and unframed: all of it goes to
    /// `stdout_data` and `stderr_data` is left empty. When `opts.stdin_data` is
    /// set those bytes are written to the exec's stdin and the send side is then
    /// half-closed so a reader (e.g. `cat`) sees EOF; on a transport that cannot
    /// half-close (TLS, or a byte-mode named pipe) this throws DockerError
    /// instead of hanging the reader. The Windows named pipe to a real daemon
    /// half-closes fine (the daemon pipe is message-mode).
    ExecResult exec(const std::string& id, const std::vector<std::string>& cmd,
                    const ExecOptions& opts);

    /// Streaming variant of `exec`: starts `cmd` with `opts` and delivers output
    /// to `consumer` incrementally as the daemon flushes it (blocking until the
    /// command finishes or `consumer` returns false). With `opts.tty == false` the
    /// stream is demuxed and each chunk is reported with its `LogSource`; with
    /// `opts.tty == true` the raw stream is reported as `LogSource::Stdout`.
    ///
    /// Returns an `ExecResult` whose `exit_code` is read from the exec inspect
    /// (0 when the command was still running after an early consumer stop —
    /// the deadline overload below reports that distinction);
    /// `stdout_data` / `stderr_data` are left empty (the output was delivered to
    /// `consumer`). `opts.detach` is rejected with a DockerError before any
    /// daemon interaction: a detached exec produces no output stream to deliver.
    ExecResult exec(const std::string& id, const std::vector<std::string>& cmd,
                    const ExecOptions& opts, const LogConsumer& consumer);

    /// Deadline-bounded streaming exec: same incremental delivery, but feeding
    /// stdin and the wait for each next output chunk are additionally bounded
    /// by the absolute `deadline` — when it passes, the stream is closed and
    /// the result says DeadlineExpired instead of blocking until the command
    /// finishes. Stopping delivery does NOT kill the command: it keeps running
    /// inside the container, so the result's `exit_code` is present only when
    /// the exec inspect says the command had finished (virtually always the
    /// case after FollowEnd::StreamEnded; after an early stop it is a race).
    /// Deadline
    /// mechanics match the follow_logs overload: connecting, the create/start
    /// round-trips, and reading the response header run under the transport's
    /// io deadline, so an expiry there surfaces as TransportTimeoutError, not
    /// as DeadlineExpired; everything after the header — the stdin writes
    /// included — is the deadline's. `opts.detach` is rejected like the
    /// overload above.
    ExecStreamResult exec(const std::string& id, const std::vector<std::string>& cmd,
                          const ExecOptions& opts, const LogConsumer& consumer,
                          std::chrono::steady_clock::time_point deadline);

    /// `PUT /containers/{id}/archive?path=/` — copy a host file, in-memory
    /// bytes, or a host directory tree into the container by extracting a tar
    /// at the root. Entry names are the target normalized (leading '/'
    /// stripped; a Windows drive-rooted target like "C:\x" becomes "x"). A
    /// single-file source needs its parent directory to already exist in the
    /// container; a directory source creates the target chain itself. The tar
    /// is produced and uploaded in blocks (a chunked request body): host files
    /// are read as they go out, so the payload is never held in memory whole.
    /// Throws DockerError on failure (non-200, or the host source cannot be
    /// read).
    void copy_to_container(const std::string& id, const CopyToContainer& source);

    /// Batched copy: ONE `PUT /containers/{id}/archive` whose tar carries the
    /// entries of all `sources` in order (later sources win on a target
    /// collision, exactly as consecutive single copies would). One round-trip
    /// and one archive regardless of the source count; an empty vector is a
    /// no-op. Streams like the single-source overload; throws like it too.
    void copy_to_container(const std::string& id, const std::vector<CopyToContainer>& sources);

    /// `GET /containers/{id}/archive?path=<container_path>` — fetch the tar archive
    /// of the file or directory at `container_path`. Returns the raw tar bytes
    /// (extract with docker::extract_tar). Throws DockerError on 404 (no such
    /// container or path) or any non-200. Buffers the whole archive — for
    /// payloads that should not sit in memory use the sink overload or
    /// `copy_from_container_to`.
    std::string copy_from_container(const std::string& id, const std::string& container_path);

    /// Streaming download: the same `GET /containers/{id}/archive`, but the
    /// raw tar bytes are delivered to `sink` in blocks as they arrive instead
    /// of being buffered. Status errors (404 -> NotFoundError) are thrown
    /// before the first block; an exception from `sink` aborts the download
    /// and propagates unchanged.
    void copy_from_container(const std::string& id, const std::string& container_path,
                             const docker::ByteSink& sink);

    /// Download the file or directory at `container_path` and EXTRACT it into
    /// the host directory `dest_dir` (created if missing), streaming: each
    /// file's bytes go from the wire straight to disk. Regular files and
    /// directories are materialized (permission bits best-effort); symlinks
    /// and other special entries are skipped; an entry that would escape
    /// `dest_dir` (absolute path or "..") throws DockerError. A single-file
    /// path lands as `dest_dir/<basename>`; a directory path lands as
    /// `dest_dir/<dirname>/...` (Docker archives are rooted at the requested
    /// path's base name).
    void copy_from_container_to(const std::string& id, const std::string& container_path,
                                const std::filesystem::path& dest_dir);

    /// Metadata of a container path, decoded from the
    /// `X-Docker-Container-Path-Stat` header of a
    /// `HEAD /containers/{id}/archive` probe — a cheap existence/size check
    /// before a download (no archive is transferred). `mode` carries Go's
    /// os.FileMode bits (type bits high, permissions low). Throws
    /// NotFoundError when the container or path does not exist.
    struct ContainerPathStat {
        std::string name;        ///< base name of the path inside the container
        std::uint64_t size = 0;  ///< file size in bytes (directory sizes are fs-defined)
        std::uint32_t mode = 0;  ///< Go os.FileMode (bit 31 = directory, low bits = perms)
        bool is_dir = false;     ///< decoded from `mode`
        std::string mtime;       ///< RFC3339 timestamp, verbatim
        std::string link_target; ///< symlink target ("" for non-links)
    };

    /// `HEAD /containers/{id}/archive?path=...` — stat a container path
    /// without downloading it (see ContainerPathStat). Throws NotFoundError
    /// on 404, DockerError on any other non-200 or an undecodable header.
    ContainerPathStat container_path_stat(const std::string& id, const std::string& container_path);

    // --- Network operations ---

    /// `POST /networks/create` — create a user-defined network, returning its id.
    /// `labels` are emitted as the network's `Labels` map (e.g. for Ryuk reaping).
    std::string create_network(const std::string& name,
                               const std::vector<std::pair<std::string, std::string>>& labels = {});

    /// `POST /networks/create` from a full spec; returns the new network id.
    std::string create_network(const NetworkCreateSpec& spec);

    /// `GET /networks/{id}` — a structured snapshot of a network (`id` may be a
    /// name or an id, as the Docker API accepts both): driver, flags, IPAM
    /// pools, options, labels, and the currently attached containers. Throws
    /// DockerError on any non-200 (NotFoundError on 404).
    NetworkInspect inspect_network(const std::string& id);

    /// `GET /networks/{id}` — the RAW inspect JSON for a network (`id` may be
    /// a name or an id, as the Docker API accepts both), so callers can read
    /// any field `NetworkInspect` does not model. Throws DockerError on any
    /// non-200 (NotFoundError on 404).
    std::string inspect_network_raw(const std::string& id);

    /// `POST /networks/{id}/connect` — attach an existing container, optionally with
    /// DNS aliases on this network.
    void connect_network(const std::string& network_id, const std::string& container_id,
                         const std::vector<std::string>& aliases = {});

    /// `POST /networks/{id}/disconnect` — detach a container from a network.
    /// `force` also detaches a container that is not running.
    void disconnect_network(const std::string& network_id, const std::string& container_id,
                            bool force = false);

    /// `DELETE /networks/{id}` — remove a network (204 expected).
    void remove_network(const std::string& id);

    // --- Volume operations ---

    /// `POST /volumes/create` — create a named volume (201 expected), returning
    /// the daemon's `Name` for it. `spec.labels` are emitted as the volume's
    /// `Labels` map (e.g. for Ryuk reaping). Throws DockerError otherwise.
    std::string create_volume(const VolumeCreateSpec& spec);

    /// `GET /volumes/{name}` — inspect a volume (200 expected). Throws DockerError
    /// on any non-200 (in particular 404 when the volume does not exist).
    VolumeInspect inspect_volume(const std::string& name);

    /// `DELETE /volumes/{name}?force=<bool>` — remove a volume (204 expected).
    /// Throws DockerError on any non-204 (404 if absent, 409 if still in use).
    void remove_volume(const std::string& name, bool force = false);

private:
    /// request() with the per-operation io deadline overridden for this one
    /// call (the long-polling endpoints widen it; nullopt disables it).
    Response
    request_with_io_timeout(std::string_view method, std::string_view target, std::string_view body,
                            const std::vector<std::pair<std::string, std::string>>& headers,
                            std::optional<std::chrono::milliseconds> io_timeout);

    /// Shared body of both copy_to_container overloads: stream the tar
    /// produced by `producer` as a chunked `PUT /containers/{id}/archive`
    /// upload and require a 200 reply. `context` prefixes error messages.
    void copy_to_archive(const std::string& id, const std::string& context,
                         const docker::BodyProducer& producer);

    /// Drop the session state, gracefully closing the cached connection (on
    /// TLS that is the close_notify exchange; plain destruction would cut it
    /// abruptly). Called by the outermost Session's destructor.
    void end_session() noexcept;

    /// Shared body of both follow_logs overloads; nullopt = no deadline
    /// (stream until the container stops or the consumer declines).
    FollowEnd follow_logs_impl(const std::string& id, const LogOptions& opts,
                               const LogConsumer& consumer,
                               std::optional<std::chrono::steady_clock::time_point> deadline);

    /// Shared body of both streaming exec overloads; nullopt = no deadline
    /// (deliver until the command finishes or the consumer declines).
    ExecStreamResult
    exec_stream_impl(const std::string& id, const std::vector<std::string>& cmd,
                     const ExecOptions& opts, const LogConsumer& consumer,
                     std::optional<std::chrono::steady_clock::time_point> deadline);

    /// The "/v1.NN" prefix every typed method pins its path with. Negotiated
    /// on first use — one unversioned `GET /_ping`, then
    /// min(kClientApiVersion, the daemon's `Api-Version` header) — and cached
    /// for the life of the instance (copies inherit it: the version belongs
    /// to the daemon). "" when the daemon reveals no parsable version: paths
    /// then stay unversioned (the daemon's default). Lazy mutation, same
    /// thread-safety rule as the session state: one instance, one thread.
    const std::string& api_prefix();

    /// `api_prefix()` + `target` — the pinned form of an Engine API path.
    std::string versioned(std::string_view target);

    DockerHost host_;
    docker::TransportTimeouts timeouts_;
    PullRetry pull_retry_;
    std::optional<std::uint64_t> max_response_body_; ///< buffered-path body cap (nullopt = none)
    std::optional<std::string> api_prefix_;          ///< negotiated "/v1.NN" ("" = none)
    bool session_enabled_ = false;                   ///< a Session is active on this instance
    std::shared_ptr<docker::ITransport> session_transport_; ///< kept-alive connection
};

} // namespace testcontainers
