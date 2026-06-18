#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/ContainerPort.hpp"

namespace testcontainers {

/// A RAII handle to a running Docker Compose project.
///
/// Compose is run via a CONTAINER-BASED AMBASSADOR: the official `docker/compose`
/// image is started inside a one-shot container with the host docker socket
/// bind-mounted (exactly how our Ryuk reaper mounts it). Real compose does the
/// orchestration against the host daemon; we bring the stack up, discover the
/// service containers by their compose labels, and expose service host ports.
/// There is NO local docker CLI and NO YAML parser here — the ambassador image
/// owns all of that.
///
/// MVP scope: a single compose file; image-based services; services that publish
/// their ports via `ports:` in the compose file (so the host can reach them).
/// See TODO.md for what is intentionally out of scope (socat ambassador for
/// unpublished ports, build contexts / host-relative volumes / `.env`,
/// multi-file compose, per-service log streaming, Ryuk reaping of the stack).
///
/// Move-only: it owns the running compose project and tears it down (compose
/// `down -v`, then a best-effort label sweep) on stop()/destruction. Copying is
/// deleted so the teardown happens exactly once.
///
/// A plain public header (no Boost/Asio/filesystem leakage): all of that lives
/// in the .cpp.
class DockerComposeContainer {
public:
    /// Build from a host compose file (read immediately; throws if unreadable).
    explicit DockerComposeContainer(const std::string& compose_file_path);

    /// Build from inline compose YAML (handy for tests).
    static DockerComposeContainer from_yaml(std::string compose_yaml);

    DockerComposeContainer(const DockerComposeContainer&) = delete;
    DockerComposeContainer& operator=(const DockerComposeContainer&) = delete;
    DockerComposeContainer(DockerComposeContainer&&) noexcept;
    DockerComposeContainer& operator=(DockerComposeContainer&&) noexcept;

    /// Best-effort stop() (never throws).
    ~DockerComposeContainer();

    // --- In-place, ref-qualified builders (set before start()) ---

    /// Record a service + container port to wait for at start(). The port must be
    /// published by the service (via `ports:`) so the host can reach it.
    DockerComposeContainer& with_exposed_service(std::string service, ContainerPort port) &;
    DockerComposeContainer&& with_exposed_service(std::string service, ContainerPort port) &&;

    /// Override the compose project name (default: "tc" + random hex).
    DockerComposeContainer& with_project_name(std::string name) &;
    DockerComposeContainer&& with_project_name(std::string name) &&;

    /// Override the ambassador image (default: "docker/compose:1.29.2").
    DockerComposeContainer& with_compose_image(std::string image) &;
    DockerComposeContainer&& with_compose_image(std::string image) &&;

    /// Bring the stack up: run the ambassador `... up -d`, wait for it to exit 0,
    /// discover the service containers by the compose project label, and wait for
    /// each with_exposed_service host port to accept a TCP connection. Throws
    /// DockerError on any failure (an ambassador non-zero exit includes its logs).
    void start();

    /// The host to reach a service on (the daemon host; "localhost" for a pipe /
    /// unix socket).
    std::string get_service_host(const std::string& service) const;

    /// The published host port mapping `service`'s container `port` (IPv4-pref).
    /// Throws if the service is unknown or the port isn't published.
    std::uint16_t get_service_port(const std::string& service, ContainerPort port) const;

    /// The discovered container id backing `service` (throws if unknown).
    std::string get_service_container_id(const std::string& service) const;

    /// Tear the stack down (ambassador `... down -v`) and remove leftovers by the
    /// project label. Idempotent; the destructor calls it best-effort.
    void stop();

    /// The compose project name (used for the project label and container names).
    const std::string& project_name() const noexcept { return project_; }

    // --- Getters (observable without a daemon; used by the unit tests) ---

    /// The compose YAML this handle owns (read from the file or set inline).
    const std::string& compose_yaml() const noexcept { return compose_yaml_; }

    /// The ambassador image used to drive compose.
    const std::string& compose_image() const noexcept { return compose_image_; }

private:
    DockerComposeContainer() = default;

    /// Best-effort teardown, swallowing any error. Marks the handle stopped.
    void drop() noexcept;

    std::string compose_yaml_;                       ///< the single compose file
    std::string project_;                            ///< compose project name
    std::string compose_image_;                      ///< ambassador image
    /// The services (+ their published port) to wait for at start().
    std::vector<std::pair<std::string, ContainerPort>> exposed_services_;
    /// compose service name -> discovered container id (filled by start()).
    std::map<std::string, std::string> service_to_id_;
    bool started_ = false;
    bool stopped_ = false;
};

} // namespace testcontainers
