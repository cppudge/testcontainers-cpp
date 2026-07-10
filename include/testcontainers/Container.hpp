#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/ExecOptions.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/Lifecycle.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/Logs.hpp"

namespace testcontainers {

namespace detail {
class Runner;
}

/// Who removes an adopted container. No default on purpose: adopting a
/// container this library did not create must state the ownership decision
/// explicitly at the call site.
enum class AdoptOwnership {
    Keep,         ///< never remove it — it is someone else's container
    RemoveOnDrop, ///< force-remove it when the handle is destroyed
};

/// A RAII handle to a running container, normally obtained from
/// `GenericImage::start()`.
///
/// Move-only: it owns a real external resource and force-removes the container
/// on destruction (best-effort, exceptions swallowed). Copying is deleted so the
/// removal happens exactly once.
///
/// A persistent handle (`remove_on_drop == false`) does NOT remove the container
/// on destruction; it is used for reusable containers (`with_reuse`), which must
/// survive across test runs, and is what `keep()` turns a normal handle into.
/// The caller is then responsible for removing the container.
class Container {
public:
    /// Adopt an already-running container that this library did NOT start (the
    /// manual escape hatch; `GenericImage::start()` is the normal way to obtain
    /// a handle). `ownership` decides whether the handle removes the container
    /// on destruction — there is deliberately no default, since destroying a
    /// container we did not create must be an explicit choice. `tty` records
    /// whether the container was created with Tty=true, so `logs()` /
    /// `follow_logs()` read its raw/unframed log stream instead of demuxing.
    /// An adopted handle does not know the exposed-port declaration order —
    /// `first_mapped_port()` falls back to the lowest-numbered published port.
    static Container adopt(DockerClient client, std::string id, AdoptOwnership ownership,
                           bool tty = false) {
        return Container(std::move(client), std::move(id),
                         ownership == AdoptOwnership::RemoveOnDrop, tty, {}, {});
    }

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    Container(Container&& other) noexcept
        : client_(std::move(other.client_)), id_(std::move(other.id_)), dropped_(other.dropped_),
          remove_on_drop_(other.remove_on_drop_), tty_(other.tty_),
          exposed_ports_(std::move(other.exposed_ports_)),
          stopping_hooks_(std::move(other.stopping_hooks_)),
          stopping_fired_(other.stopping_fired_) {
        other.dropped_ = true;        // the moved-from handle owns nothing
        other.stopping_fired_ = true; // ...and must never fire the stopping hooks
    }

    Container& operator=(Container&& other) noexcept {
        if (this != &other) {
            drop();
            client_ = std::move(other.client_);
            id_ = std::move(other.id_);
            dropped_ = other.dropped_;
            remove_on_drop_ = other.remove_on_drop_;
            tty_ = other.tty_;
            exposed_ports_ = std::move(other.exposed_ports_);
            stopping_hooks_ = std::move(other.stopping_hooks_);
            stopping_fired_ = other.stopping_fired_;
            other.dropped_ = true;
            other.stopping_fired_ = true;
        }
        return *this;
    }

    /// Force-removes the container unless it was already explicitly removed or
    /// moved-from. Never throws.
    ~Container() { drop(); }

    /// The full container id.
    const std::string& id() const noexcept { return id_; }

    /// True when this is a persistent (reusable) handle that will NOT remove the
    /// container on destruction.
    bool is_persistent() const noexcept { return !remove_on_drop_; }

    /// True when the container was created with a pseudo-TTY (Tty=true), in which
    /// case its log stream is raw/unframed (no separate stderr channel).
    bool has_tty() const noexcept { return tty_; }

    /// The address a client on this host should connect to ("localhost" for a
    /// unix socket / named pipe, otherwise the daemon hostname).
    std::string host() const { return client_.host().http_host(); }

    /// The host port Docker published for the given container port. Prefers the
    /// IPv4 binding, falling back to the first (e.g. IPv6-only) binding. Throws
    /// DockerError if the port is not exposed/published.
    std::uint16_t get_host_port(ContainerPort port) const;

    /// The host port Docker published on an IPv4 address (host_ip "0.0.0.0" or
    /// empty) for `port`. Throws DockerError if there is no IPv4 binding.
    std::uint16_t get_host_port_ipv4(ContainerPort port) const;

    /// The host port Docker published on an IPv6 address (host_ip contains ':')
    /// for `port`. Throws DockerError if there is no IPv6 binding.
    std::uint16_t get_host_port_ipv6(ContainerPort port) const;

    /// The mapped host port of the container's FIRST exposed port (the order
    /// passed to `GenericImage::with_exposed_port`); convenience for the common
    /// single-port container. IPv4 binding preferred. When the exposed-port order
    /// is unknown (e.g. an adopted / manually-constructed handle) this falls back
    /// to the lowest-numbered published container port. Throws DockerError if the
    /// container publishes no ports.
    std::uint16_t first_mapped_port() const;

    /// A structured snapshot of the container (`GET /containers/{id}/json`).
    ContainerInspect inspect() const;

    /// The RAW inspect JSON body (`GET /containers/{id}/json`), so callers can
    /// read any field `ContainerInspect` does not model. Throws DockerError if the
    /// container is gone.
    std::string inspect_raw() const;

    /// A structured snapshot of an arbitrary container by id (or name), without
    /// a Container handle — a read-only lookup that, unlike `adopt`, takes no
    /// ownership. Connects via `DockerClient::from_environment()`. Throws
    /// DockerError if no such container exists (NotFoundError) or the daemon
    /// cannot be reached.
    static ContainerInspect inspect(const std::string& id);

    /// A snapshot of the container's stdout / stderr logs.
    ContainerLogs logs() const;

    /// Stream this container's logs to `consumer` until the container stops or the
    /// consumer returns false. Blocking — run on your own std::thread for background
    /// consumption. See DockerClient::follow_logs.
    void follow_logs(const LogConsumer& consumer, const LogOptions& opts = {}) const;

    /// Run a command inside the running container, capturing its stdout / stderr
    /// and exit code.
    ExecResult exec(const std::vector<std::string>& cmd) const;

    /// Run a command inside the running container with `opts` (env / working dir /
    /// user / privileged / tty / stdin / detach), capturing its output and exit
    /// code. See DockerClient::exec for the tty/stdin/detach semantics.
    ExecResult exec(const std::vector<std::string>& cmd, const ExecOptions& opts) const;

    /// Streaming variant: run `cmd` with `opts`, delivering output to `consumer`
    /// incrementally and returning an ExecResult with the exit code set (the
    /// stdout/stderr fields are left empty — the output went to `consumer`). See
    /// DockerClient::exec.
    ExecResult exec(const std::vector<std::string>& cmd, const ExecOptions& opts,
                    const LogConsumer& consumer) const;

    /// Deadline-bounded streaming variant: like the consumer overload, but
    /// feeding stdin and the wait for each next output chunk are bounded by
    /// the absolute `deadline` — when it passes, delivery stops with
    /// FollowEnd::DeadlineExpired instead of blocking until the command
    /// finishes. The command is NOT killed by that: it keeps running inside
    /// the container, and the result's exit_code is present only when it had
    /// actually finished. See DockerClient::exec.
    ExecStreamResult exec(const std::vector<std::string>& cmd, const ExecOptions& opts,
                          const LogConsumer& consumer,
                          std::chrono::steady_clock::time_point deadline) const;

    /// Copy a host file, in-memory bytes, or a host directory tree into this
    /// already-running container (`PUT /containers/{id}/archive`). For a
    /// single-file source the target's parent directory must already exist; a
    /// directory source creates the target directory chain itself. Throws
    /// DockerError on failure.
    void copy_to(const CopyToContainer& source) const;

    /// Read a single regular file out of the container and return its bytes.
    /// Fetches `GET .../archive` for `container_path` and extracts the one regular
    /// file in the archive. Throws DockerError if the path is not a single regular
    /// file (e.g. a directory). The bytes may be binary.
    std::string read_file(const std::string& container_path) const;

    /// Copy a single regular file out of the container to `host_dest` (a host
    /// filesystem path; its parent directory is created if missing). Throws
    /// DockerError on failure. For directory trees use copy_from_container +
    /// extract_tar directly.
    void copy_file_from(const std::string& container_path,
                        const std::filesystem::path& host_dest) const;

    /// Stop the container (an auto-removing handle still removes it on
    /// destruction; a persistent handle — `with_reuse` / after `keep()` —
    /// does not).
    void stop();

    /// Whether the container is currently running (per a fresh inspect).
    bool is_running() const;

    /// Keep the container alive past this handle: from here on neither
    /// destruction nor `remove()` removes it, and the stopping hooks do not
    /// fire on drop — the handle becomes persistent, exactly as if the
    /// container had been started `with_reuse` (`is_persistent()` reports true
    /// afterwards). Removing the container then becomes the caller's
    /// responsibility (e.g. `DockerClient::remove_container` or `docker rm -f`).
    ///
    /// `keep(false)` re-arms removal — handy for forwarding a "keep my
    /// containers" debug flag in one call instead of an `if`. It also works on
    /// a `with_reuse` handle, but then THIS handle removes the shared
    /// container on drop, defeating reuse for later runs — rarely what you
    /// want.
    ///
    /// Ryuk still applies on Linux engines: a normally-started container
    /// carries the session label, so the reaper removes it shortly after the
    /// test process exits. keep() only protects it from THIS process's
    /// teardown — for a container that must outlive the process, disable the
    /// reaper (TESTCONTAINERS_RYUK_DISABLED) or use `with_reuse`, whose
    /// containers are never session-labeled. (No reaper runs against a
    /// Windows-containers engine — a kept container there stays until you
    /// remove it.)
    void keep(bool keep = true) noexcept { remove_on_drop_ = !keep; }

    /// Explicitly stop owning / force-remove the container now. Idempotent;
    /// after this the destructor does nothing. On a persistent handle
    /// (`with_reuse` / after `keep()`) this releases ownership without removing
    /// the container.
    void remove();

private:
    // The constructor below is the start orchestration's (the friend's) private
    // channel for handing over a fully-configured handle; it is not part of
    // the user-facing API. detail::Runner is what GenericImage::start() / the
    // free run() ultimately drive.
    friend class detail::Runner;

    /// Wrap an already-created, already-started container. The wiring is
    /// constructor-only so a handle is never observable half-configured:
    /// `stopping_hooks` fire once at teardown (on stop()/remove(), or on
    /// destruction of an auto-removing handle — never on a persistent handle's
    /// drop); `exposed_ports` records the user's declaration order so
    /// `first_mapped_port()` resolves the FIRST exposed port instead of
    /// guessing the lowest-numbered published one.
    Container(DockerClient client, std::string id, bool remove_on_drop, bool tty,
              std::vector<LifecycleHook> stopping_hooks, std::vector<ContainerPort> exposed_ports)
        : client_(std::move(client)), id_(std::move(id)), remove_on_drop_(remove_on_drop),
          tty_(tty), exposed_ports_(std::move(exposed_ports)),
          stopping_hooks_(std::move(stopping_hooks)) {}

    /// Best-effort force-remove, swallowing any error. Marks the handle dropped.
    void drop() noexcept;

    /// Fire the stopping hooks exactly once, swallowing any exception (teardown
    /// is best-effort and must never propagate, especially from the destructor).
    void fire_stopping() noexcept;

    // Mutable: the client is just a stateless host config that opens a fresh
    // connection per call, so issuing requests through it is logically const
    // from the handle's point of view (const accessors like get_host_port /
    // is_running / logs need it).
    mutable DockerClient client_;
    std::string id_;
    bool dropped_ = false;
    bool remove_on_drop_ = true; ///< false for persistent (reusable) handles
    bool tty_ = false;           ///< container was created with Tty=true (raw log stream)
    /// Exposed ports in declared order (for first_mapped_port).
    std::vector<ContainerPort> exposed_ports_;
    std::vector<LifecycleHook> stopping_hooks_; ///< fired once at teardown
    bool stopping_fired_ = false;               ///< guard: stopping hooks fired exactly once
};

} // namespace testcontainers
