#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/ContainerPort.hpp"

namespace testcontainers {

namespace compose {
class IComposeClient; // internal; defined under src/compose
} // namespace compose

/// Which compose client drives the project (mirrors testcontainers-rs's factory
/// naming `with_local_client` / `with_containerised_client` / `with_auto_client`).
enum class ComposeClientKind {
    Local,         ///< host `docker compose` CLI (THE DEFAULT, matching rust)
    Containerised, ///< `docker compose` inside a long-lived `docker:cli` container
    Auto,          ///< probe local; fall back to containerised
};

/// A RAII handle to a running Docker Compose project.
///
/// THREE client modes (rust parity), selected by ComposeClientKind:
///   - Local (DEFAULT): shells out to the host `docker compose` CLI. This is a
///     DELIBERATE, COMPOSE-ONLY exception to the library's "no docker CLI" rule
///     (the rest of the library stays pure Engine-API). rust's default is the
///     local client, so ours is too.
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
    /// Build from host compose files (default client = Local, matching rust).
    /// The files are referenced by absolute path at start() (they are not read
    /// here); for containerised mode they are copied into the cli container.
    explicit DockerComposeContainer(std::vector<std::string> compose_files);

    /// Single-file convenience.
    explicit DockerComposeContainer(const std::string& compose_file);

    // --- rust-parity factories (set files + client kind) ---

    static DockerComposeContainer with_local_client(std::vector<std::string> compose_files);
    static DockerComposeContainer with_containerised_client(std::vector<std::string> compose_files);
    static DockerComposeContainer with_auto_client(std::vector<std::string> compose_files);

    /// Inline YAML convenience: writes a temp `.yml` file (cleaned up on
    /// destruction) and uses it as the single compose file; default Local.
    static DockerComposeContainer from_yaml(std::string compose_yaml);

    DockerComposeContainer(const DockerComposeContainer&) = delete;
    DockerComposeContainer& operator=(const DockerComposeContainer&) = delete;
    DockerComposeContainer(DockerComposeContainer&&) noexcept;
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
    /// service (via `ports:`) so the host can reach it.
    DockerComposeContainer& with_exposed_service(std::string service, ContainerPort port) &;
    DockerComposeContainer&& with_exposed_service(std::string service, ContainerPort port) &&;

    /// Override the compose project name (default: "tc" + random hex).
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

    /// Timeout for compose `--wait` (`--wait-timeout <secs>`). Default 60s.
    DockerComposeContainer& with_wait_timeout(std::chrono::seconds timeout) &;
    DockerComposeContainer&& with_wait_timeout(std::chrono::seconds timeout) &&;

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
    /// unix socket).
    std::string get_service_host(const std::string& service) const;

    /// The published host port mapping `service`'s container `port` (IPv4-pref).
    /// Throws if the service is unknown or the port isn't published.
    std::uint16_t get_service_port(const std::string& service, ContainerPort port) const;

    /// The discovered container id backing `service` (throws if unknown).
    std::string get_service_container_id(const std::string& service) const;

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

    /// Best-effort teardown, swallowing any error. Marks the handle stopped.
    void drop() noexcept;

    std::vector<std::string> compose_files_;         ///< host compose files
    std::string project_;                            ///< compose project name
    std::string compose_image_;                      ///< containerised ambassador image
    ComposeClientKind client_kind_ = ComposeClientKind::Local;
    std::map<std::string, std::string> env_;         ///< compose env vars
    bool build_ = false;
    bool pull_ = false;
    bool wait_ = true;
    std::chrono::seconds wait_timeout_{60};
    bool remove_volumes_ = true;
    bool remove_images_ = false;
    /// The services (+ their published port) to wait for at start().
    std::vector<std::pair<std::string, ContainerPort>> exposed_services_;
    /// compose service name -> discovered container id (filled by start()).
    std::map<std::string, std::string> service_to_id_;
    /// A temp file written by from_yaml(), deleted on destruction (else empty).
    std::string temp_file_;
    /// The resolved compose client (created in start(); owns the containerised
    /// cli container, so it must live until stop()/destruction). Opaque here.
    std::unique_ptr<compose::IComposeClient> client_;
    bool started_ = false;
    bool stopped_ = false;
};

} // namespace testcontainers
