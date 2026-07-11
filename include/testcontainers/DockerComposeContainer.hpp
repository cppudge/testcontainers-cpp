#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/docker/Logs.hpp"

namespace testcontainers {

/// Which compose client drives the project.
enum class ComposeClientKind {
    Local,         ///< host `docker compose` CLI (the default)
    Containerised, ///< `docker compose` inside a long-lived `docker:cli` container
    Auto,          ///< probe local; fall back to containerised
};

/// A RAII handle to a running Docker Compose project.
///
/// Three client modes, selected by ComposeClientKind:
///   - Local (DEFAULT): shells out to the host `docker compose` CLI. This is a
///     DELIBERATE, COMPOSE-ONLY exception to the library's "no docker CLI" rule
///     (the rest of the library stays pure Engine-API).
///   - Containerised: runs `docker compose` inside ONE long-lived `docker:cli`
///     container (entrypoint /bin/sh, cmd `-c sleep infinity`) with the host
///     docker socket bind-mounted; each compose file is copied in and `up`/`down`
///     are exec'd into it. No host docker CLI required.
///   - Auto: probes `docker compose version`; uses Local on exit 0, else
///     Containerised.
///
/// Readiness uses compose v2's native `--wait --wait-timeout <secs>` (default
/// on, 60s) PLUS — as an extra guarantee — the existing per-`with_exposed_service`
/// TCP-connect probe after `up`, which confirms the published port is actually
/// open even for services without a healthcheck.
///
/// Discovery + teardown is unchanged: after `up` we discover service containers
/// by the `com.docker.compose.project` label; on stop()/destruction we run
/// compose `down` and a best-effort project-label sweep.
///
/// Move-only: it owns the running compose project (and, in containerised mode,
/// the long-lived cli container) and tears everything down on stop()/destruction.
///
/// A plain public header (no Boost/Asio/subprocess/filesystem leakage): all of
/// that lives in the .cpp and the internal src/compose/ files.
class DockerComposeContainer {
public:
    /// Build from host compose files (default client = Local).
    /// The files are referenced by absolute path at start() (they are not read
    /// here); for containerised mode they are copied into the cli container.
    explicit DockerComposeContainer(std::vector<std::string> compose_files);

    /// Single-file convenience.
    explicit DockerComposeContainer(const std::string& compose_file);

    // --- Factories (set files + client kind) ---

    static DockerComposeContainer with_local_client(std::vector<std::string> compose_files);
    static DockerComposeContainer with_containerised_client(std::vector<std::string> compose_files);
    static DockerComposeContainer with_auto_client(std::vector<std::string> compose_files);

    /// Inline YAML convenience: writes a temp `.yml` file (cleaned up on
    /// destruction) and uses it as the single compose file; default Local.
    static DockerComposeContainer from_yaml(const std::string& compose_yaml);

    DockerComposeContainer(const DockerComposeContainer&) = delete;
    DockerComposeContainer& operator=(const DockerComposeContainer&) = delete;
    /// A moved-from handle owns nothing: destroying it tears nothing down.
    DockerComposeContainer(DockerComposeContainer&&) noexcept;
    /// Releases the target's own stack/temp file before adopting the source's.
    DockerComposeContainer& operator=(DockerComposeContainer&&) noexcept;

    /// Best-effort stop() (never throws).
    ~DockerComposeContainer();

    // --- In-place, ref-qualified builders (set before start()) ---

    /// Override the client kind on an existing instance (so e.g.
    /// `from_yaml(...).with_client(Containerised)` works).
    DockerComposeContainer& with_client(ComposeClientKind kind) &;
    DockerComposeContainer&& with_client(ComposeClientKind kind) &&;

    /// Record a service + container port to wait for at start() (an EXTRA TCP
    /// probe on top of compose `--wait`). The port must be published by the
    /// service (via `ports:`) so the host can reach it. For a scaled service
    /// this probes the first (lowest-numbered) instance; the three-argument
    /// overload probes a specific one.
    DockerComposeContainer& with_exposed_service(std::string service, ContainerPort port) &;
    DockerComposeContainer&& with_exposed_service(std::string service, ContainerPort port) &&;
    DockerComposeContainer& with_exposed_service(std::string service, int instance,
                                                 ContainerPort port) &;
    DockerComposeContainer&& with_exposed_service(std::string service, int instance,
                                                  ContainerPort port) &&;

    /// Run `instances` containers of `service` (`up --scale <service>=<n>`;
    /// repeatable — the last value per service wins; overrides the file's
    /// `deploy.replicas`). Scaling to 0 starts none. Instances are numbered
    /// from 1 and selected via the instance-taking accessor overloads.
    ///
    /// A service cannot scale past 1 while publishing a FIXED host port — the
    /// instances would collide over it. Publish the container port alone
    /// (e.g. `- "6379"`) so each instance gets its own ephemeral host port.
    DockerComposeContainer& with_scale(std::string service, int instances) &;
    DockerComposeContainer&& with_scale(std::string service, int instances) &&;

    /// Activate a compose profile (repeatable). Services carrying `profiles:`
    /// in the YAML only start when one of their profiles is active; profile-less
    /// services always start. The profiles stay active for the teardown `down`
    /// too, so profile-gated services are removed with the rest of the stack.
    /// Prefer this over setting `COMPOSE_PROFILES` via with_env: when both are
    /// given they do not merge, and which one wins varies by compose version.
    DockerComposeContainer& with_profile(std::string profile) &;
    DockerComposeContainer&& with_profile(std::string profile) &&;

    /// Override the compose project name (default: "tc" + random hex).
    ///
    /// The project is registered with the Ryuk reaper at start(): EVERYTHING
    /// carrying this `com.docker.compose.project` label — containers, project
    /// networks and volumes — is removed shortly after the test process exits.
    /// Do not reuse the name of a compose stack that must outlive the tests.
    DockerComposeContainer& with_project_name(std::string name) &;
    DockerComposeContainer&& with_project_name(std::string name) &&;

    /// Override the containerised ambassador image (default: "docker:26.1-cli").
    /// Ignored by the Local client.
    DockerComposeContainer& with_compose_image(std::string image) &;
    DockerComposeContainer&& with_compose_image(std::string image) &&;

    /// Set a single environment variable on the compose invocation.
    DockerComposeContainer& with_env(std::string key, std::string value) &;
    DockerComposeContainer&& with_env(std::string key, std::string value) &&;

    /// Set environment variables in bulk (merged over any already set).
    DockerComposeContainer& with_env_vars(std::map<std::string, std::string> env) &;
    DockerComposeContainer&& with_env_vars(std::map<std::string, std::string> env) &&;

    /// Build images before starting (`--build`). Default off.
    DockerComposeContainer& with_build(bool build = true) &;
    DockerComposeContainer&& with_build(bool build = true) &&;

    /// Pull images before starting (`--pull always`). Default off.
    DockerComposeContainer& with_pull(bool pull = true) &;
    DockerComposeContainer&& with_pull(bool pull = true) &&;

    /// Use compose's native `--wait` for readiness. Default ON.
    DockerComposeContainer& with_wait(bool wait = true) &;
    DockerComposeContainer&& with_wait(bool wait = true) &&;

    /// Readiness timeout, default 60s. Applies to compose `--wait`
    /// (`--wait-timeout <secs>`) AND to each exposed service's TCP probe; the
    /// phases are budgeted separately (each gets the full timeout), not shared
    /// under one deadline.
    DockerComposeContainer& with_wait_timeout(std::chrono::seconds timeout) &;
    DockerComposeContainer&& with_wait_timeout(std::chrono::seconds timeout) &&;

    // Teardown behavior (env, remove_volumes, remove_images) is snapshotted
    // by start(): changing these afterwards does not affect the pending
    // teardown — consistent with the "set before start()" contract above.

    /// Remove volumes on teardown (`down --volumes`). Default true.
    DockerComposeContainer& with_remove_volumes(bool remove = true) &;
    DockerComposeContainer&& with_remove_volumes(bool remove = true) &&;

    /// Remove images on teardown (`down --rmi all`). Default false.
    DockerComposeContainer& with_remove_images(bool remove = true) &;
    DockerComposeContainer&& with_remove_images(bool remove = true) &&;

    /// Bring the stack up: resolve the client, run compose `up -d ... --wait`,
    /// discover the service containers by the compose project label, and wait
    /// for each with_exposed_service host port to accept a TCP connection.
    /// Throws DockerError on any failure (a non-zero compose exit includes its
    /// output).
    void start();

    /// The host to reach a service on (the daemon host; "localhost" for a pipe /
    /// unix socket — the same for every instance).
    std::string get_service_host(const std::string& service) const;

    /// The published host port mapping `service`'s container `port` (IPv4-pref),
    /// on the first (lowest-numbered) instance. Throws if the service is
    /// unknown or the port isn't published.
    std::uint16_t get_service_port(const std::string& service, ContainerPort port) const;

    /// The published host port mapping container `port` on instance `instance`
    /// (numbered from 1) of a scaled `service`. Throws if the service or the
    /// instance is unknown, or the port isn't published.
    std::uint16_t get_service_port(const std::string& service, int instance,
                                   ContainerPort port) const;

    /// The discovered container id backing `service` — the first
    /// (lowest-numbered) instance (throws if unknown).
    std::string get_service_container_id(const std::string& service) const;

    /// The discovered container id backing instance `instance` (numbered from
    /// 1) of a scaled `service`. Throws if the service or the instance is
    /// unknown (the error lists the running instance numbers).
    std::string get_service_container_id(const std::string& service, int instance) const;

    /// The running instance numbers of `service`, ascending (a single-instance
    /// service yields {1}). Throws if the service is unknown.
    std::vector<int> service_instances(const std::string& service) const;

    /// A snapshot of `service`'s stdout / stderr logs — the first
    /// (lowest-numbered) instance's. A service whose YAML sets `tty: true`
    /// writes a raw/unframed stream: set `opts.tty` to read it. Throws if the
    /// service is unknown.
    ///
    /// Pass options as a spelled-out `LogOptions{...}` (or omit them) in every
    /// log accessor: a bare `{}` in the options slot reads as `int` 0 to
    /// overload resolution and selects the instance form here — and is
    /// ambiguous against the deadline in the follow forms.
    ContainerLogs get_service_logs(const std::string& service, const LogOptions& opts = {}) const;

    /// The log snapshot of instance `instance` (numbered from 1) of a scaled
    /// `service`.
    ContainerLogs get_service_logs(const std::string& service, int instance,
                                   const LogOptions& opts = {}) const;

    /// Stream `service`'s logs (first instance) to `consumer` until the
    /// container stops or the consumer returns false. Blocking — run on your
    /// own thread for background consumption. See DockerClient::follow_logs.
    void follow_service_logs(const std::string& service, const LogConsumer& consumer,
                             const LogOptions& opts = {}) const;

    /// The instance-selecting form of the blocking stream above.
    void follow_service_logs(const std::string& service, int instance, const LogConsumer& consumer,
                             const LogOptions& opts = {}) const;

    /// Deadline-bounded stream of `service`'s logs (first instance): also stops
    /// when `deadline` passes, returning why the stream ended instead of
    /// blocking until the container stops. See the DockerClient overload for
    /// the deadline mechanics.
    FollowEnd follow_service_logs(const std::string& service, const LogConsumer& consumer,
                                  std::chrono::steady_clock::time_point deadline,
                                  const LogOptions& opts = {}) const;

    /// The instance-selecting form of the deadline-bounded stream above.
    FollowEnd follow_service_logs(const std::string& service, int instance,
                                  const LogConsumer& consumer,
                                  std::chrono::steady_clock::time_point deadline,
                                  const LogOptions& opts = {}) const;

    /// Tear the stack down (compose `down`) and remove leftovers by the project
    /// label. Idempotent; the destructor calls it best-effort.
    void stop();

    /// The compose project name (used for the project label and container names).
    const std::string& project_name() const noexcept { return project_; }

    // --- Getters (observable without a daemon; used by the unit tests) ---

    /// The compose files this handle drives (host paths).
    const std::vector<std::string>& compose_files() const noexcept { return compose_files_; }

    /// The selected client kind (default Local).
    ComposeClientKind client_kind() const noexcept { return client_kind_; }

    /// The containerised ambassador image.
    const std::string& compose_image() const noexcept { return compose_image_; }

    /// The environment variables set on the compose invocation.
    const std::map<std::string, std::string>& env() const noexcept { return env_; }

    /// The active compose profiles, in the order added.
    const std::vector<std::string>& profiles() const noexcept { return profiles_; }

    /// The requested per-service instance counts (service -> n).
    const std::map<std::string, int>& scales() const noexcept { return scales_; }

    /// `--build` flag.
    bool build() const noexcept { return build_; }
    /// `--pull always` flag.
    bool pull() const noexcept { return pull_; }
    /// `--wait` flag.
    bool wait() const noexcept { return wait_; }
    /// `--wait-timeout` value.
    std::chrono::seconds wait_timeout() const noexcept { return wait_timeout_; }
    /// `down --volumes` flag.
    bool remove_volumes() const noexcept { return remove_volumes_; }
    /// `down --rmi all` flag.
    bool remove_images() const noexcept { return remove_images_; }

private:
    DockerComposeContainer() = default;

    /// RAII owner of the from_yaml temp file: the file is deleted when the
    /// owner is destroyed, assigned over, or remove()d; moving leaves the
    /// source owning nothing. File operations live in the .cpp so the header
    /// stays <filesystem>-free.
    class TempFile {
    public:
        TempFile() = default;
        explicit TempFile(std::string path) noexcept : path_(std::move(path)) {}
        TempFile(TempFile&& other) noexcept : path_(std::move(other.path_)) { other.path_.clear(); }
        TempFile& operator=(TempFile&& other) noexcept; // deletes the current file first
        TempFile(const TempFile&) = delete;
        TempFile& operator=(const TempFile&) = delete;
        ~TempFile();
        /// The owned path (empty when none).
        const std::string& path() const noexcept { return path_; }
        /// Delete the file now (best-effort) and forget it.
        void remove() noexcept;

    private:
        std::string path_;
    };

    /// The instance-number -> container-id map for `service` (never empty);
    /// throws the unknown-service DockerError when absent.
    const std::map<int, std::string>& find_service_instances(const std::string& service) const;

    std::vector<std::string> compose_files_; ///< host compose files
    std::string project_;                    ///< compose project name
    std::string compose_image_;              ///< containerised ambassador image
    ComposeClientKind client_kind_ = ComposeClientKind::Local;
    std::map<std::string, std::string> env_; ///< compose env vars
    std::vector<std::string> profiles_;      ///< active compose profiles
    std::map<std::string, int> scales_;      ///< service -> --scale count
    bool build_ = false;
    bool pull_ = false;
    bool wait_ = true;
    std::chrono::seconds wait_timeout_{60};
    bool remove_volumes_ = true;
    bool remove_images_ = false;
    /// A with_exposed_service entry: which instance's published port to probe.
    struct ExposedService {
        std::string service;
        int instance; ///< 0 = the first (lowest-numbered) instance
        ContainerPort port;
    };
    /// The services (+ their published port) to wait for at start().
    std::vector<ExposedService> exposed_services_;
    /// The running project (created by start()): the compose client, a
    /// teardown snapshot of the config, and the discovered service ids.
    /// Destroying it IS the teardown, so the defaulted moves inherit correct
    /// transfer/release semantics from unique_ptr.
    ///
    /// Ordering invariant: teardown may re-read the compose file (`down -f`),
    /// so it must run BEFORE temp_file_ deletes it. active_ is declared first
    /// so the defaulted move-ASSIGN releases the target's stack before its
    /// temp file; the destructor calls stop(), which encodes the same order
    /// explicitly (plain member destruction would run it in reverse).
    struct ActiveStack;
    std::unique_ptr<ActiveStack> active_;
    /// The temp file written by from_yaml() (deleted by stop()/destruction).
    TempFile temp_file_;
};

} // namespace testcontainers
