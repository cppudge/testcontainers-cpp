#include "testcontainers/DockerComposeContainer.hpp"

#include "RandomHex.hpp"
#include "compose/ComposeClients.hpp"
#include "compose/ComposeCommand.hpp"
#include "docker/Ports.hpp"

#include "testcontainers/Error.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace testcontainers {

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

/// The default containerised ambassador image: a long-lived `docker:cli`
/// container drives `docker compose` (v2) via exec. (Was `docker/compose:1.29.2`
/// in the one-shot MVP.)
constexpr const char* kComposeImage = "docker:26.1-cli";

constexpr const char* kComposeProjectLabel = "com.docker.compose.project";
constexpr const char* kComposeServiceLabel = "com.docker.compose.service";

using detail::random_hex;

/// Resolve the published host port for `key` (e.g. "6379/tcp") in an inspect,
/// preferring the IPv4 binding (same policy as Container::get_host_port).
std::uint16_t published_host_port(const ContainerInspect& info, const std::string& key) {
    const auto host_port =
        docker::select_host_port(info.ports, key, docker::HostPortFamily::Any);
    if (!host_port) {
        throw DockerError("Compose service container " + info.id +
                          " published no host port for " + key);
    }
    return *host_port;
}

/// Map the public client kind to the internal compose-client kind.
compose::ClientKind to_internal_kind(ComposeClientKind kind) {
    switch (kind) {
    case ComposeClientKind::Local:
        return compose::ClientKind::Local;
    case ComposeClientKind::Containerised:
        return compose::ClientKind::Containerised;
    case ComposeClientKind::Auto:
        return compose::ClientKind::Auto;
    }
    return compose::ClientKind::Local;
}

/// Absolutize a host path (compose `-f` and `--project-directory` want absolute
/// paths so they work regardless of the child process's working dir).
std::string absolute_path(const std::string& path) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(path, ec);
    if (ec) {
        return path;
    }
    return abs.string();
}

} // namespace

DockerComposeContainer::DockerComposeContainer(std::vector<std::string> files)
    : compose_files_(std::move(files)), project_("tc" + random_hex(8)),
      compose_image_(kComposeImage) {
    // compose `-f` wants absolute paths; absolutize in place (reusing the moved
    // vector's buffer) rather than building a second vector element by element.
    for (std::string& f : compose_files_) {
        f = absolute_path(f);
    }
}

DockerComposeContainer::DockerComposeContainer(const std::string& compose_file)
    : DockerComposeContainer(std::vector<std::string>{compose_file}) {}

DockerComposeContainer
DockerComposeContainer::with_local_client(std::vector<std::string> compose_files) {
    DockerComposeContainer c(std::move(compose_files));
    c.client_kind_ = ComposeClientKind::Local;
    return c;
}

DockerComposeContainer
DockerComposeContainer::with_containerised_client(std::vector<std::string> compose_files) {
    DockerComposeContainer c(std::move(compose_files));
    c.client_kind_ = ComposeClientKind::Containerised;
    return c;
}

DockerComposeContainer
DockerComposeContainer::with_auto_client(std::vector<std::string> compose_files) {
    DockerComposeContainer c(std::move(compose_files));
    c.client_kind_ = ComposeClientKind::Auto;
    return c;
}

DockerComposeContainer DockerComposeContainer::from_yaml(std::string compose_yaml) {
    DockerComposeContainer c;
    c.project_ = "tc" + random_hex(8);
    c.compose_image_ = kComposeImage;
    c.client_kind_ = ComposeClientKind::Local;

    // Write the inline YAML to a temp `.yml` so the (default) local client has a
    // real file. Recorded for deletion in the destructor.
    const std::filesystem::path temp =
        std::filesystem::temp_directory_path() / ("tc-compose-" + random_hex(12) + ".yml");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw DockerError("DockerComposeContainer::from_yaml: cannot create temp file '" +
                              temp.string() + "'");
        }
        out << compose_yaml;
    }
    c.temp_file_ = temp.string();
    c.compose_files_ = {c.temp_file_};
    return c;
}

DockerComposeContainer::DockerComposeContainer(DockerComposeContainer&& other) noexcept
    : compose_files_(std::move(other.compose_files_)), project_(std::move(other.project_)),
      compose_image_(std::move(other.compose_image_)), client_kind_(other.client_kind_),
      env_(std::move(other.env_)), build_(other.build_), pull_(other.pull_), wait_(other.wait_),
      wait_timeout_(other.wait_timeout_), remove_volumes_(other.remove_volumes_),
      remove_images_(other.remove_images_), exposed_services_(std::move(other.exposed_services_)),
      service_to_id_(std::move(other.service_to_id_)), temp_file_(std::move(other.temp_file_)),
      client_(std::move(other.client_)), started_(other.started_), stopped_(other.stopped_) {
    other.temp_file_.clear(); // the moved-from handle owns no temp file
    other.started_ = false;
    other.stopped_ = true; // the moved-from handle owns nothing — never tear down
}

DockerComposeContainer& DockerComposeContainer::operator=(DockerComposeContainer&& other) noexcept {
    if (this != &other) {
        drop();
        compose_files_ = std::move(other.compose_files_);
        project_ = std::move(other.project_);
        compose_image_ = std::move(other.compose_image_);
        client_kind_ = other.client_kind_;
        env_ = std::move(other.env_);
        build_ = other.build_;
        pull_ = other.pull_;
        wait_ = other.wait_;
        wait_timeout_ = other.wait_timeout_;
        remove_volumes_ = other.remove_volumes_;
        remove_images_ = other.remove_images_;
        exposed_services_ = std::move(other.exposed_services_);
        service_to_id_ = std::move(other.service_to_id_);
        temp_file_ = std::move(other.temp_file_);
        client_ = std::move(other.client_);
        started_ = other.started_;
        stopped_ = other.stopped_;
        other.temp_file_.clear();
        other.started_ = false;
        other.stopped_ = true;
    }
    return *this;
}

DockerComposeContainer::~DockerComposeContainer() { drop(); }

DockerComposeContainer& DockerComposeContainer::with_client(ComposeClientKind kind) & {
    client_kind_ = kind;
    return *this;
}
DockerComposeContainer&& DockerComposeContainer::with_client(ComposeClientKind kind) && {
    client_kind_ = kind;
    return std::move(*this);
}

DockerComposeContainer& DockerComposeContainer::with_exposed_service(std::string service,
                                                                     ContainerPort port) & {
    exposed_services_.emplace_back(std::move(service), port);
    return *this;
}
DockerComposeContainer&& DockerComposeContainer::with_exposed_service(std::string service,
                                                                      ContainerPort port) && {
    exposed_services_.emplace_back(std::move(service), port);
    return std::move(*this);
}

DockerComposeContainer& DockerComposeContainer::with_project_name(std::string name) & {
    project_ = std::move(name);
    return *this;
}
DockerComposeContainer&& DockerComposeContainer::with_project_name(std::string name) && {
    project_ = std::move(name);
    return std::move(*this);
}

DockerComposeContainer& DockerComposeContainer::with_compose_image(std::string image) & {
    compose_image_ = std::move(image);
    return *this;
}
DockerComposeContainer&& DockerComposeContainer::with_compose_image(std::string image) && {
    compose_image_ = std::move(image);
    return std::move(*this);
}

DockerComposeContainer& DockerComposeContainer::with_env(std::string key, std::string value) & {
    env_[std::move(key)] = std::move(value);
    return *this;
}
DockerComposeContainer&& DockerComposeContainer::with_env(std::string key, std::string value) && {
    env_[std::move(key)] = std::move(value);
    return std::move(*this);
}

DockerComposeContainer&
DockerComposeContainer::with_env_vars(std::map<std::string, std::string> env) & {
    for (auto& [key, value] : env) {
        env_[key] = std::move(value);
    }
    return *this;
}
DockerComposeContainer&&
DockerComposeContainer::with_env_vars(std::map<std::string, std::string> env) && {
    for (auto& [key, value] : env) {
        env_[key] = std::move(value);
    }
    return std::move(*this);
}

DockerComposeContainer& DockerComposeContainer::with_build(bool build) & {
    build_ = build;
    return *this;
}
DockerComposeContainer&& DockerComposeContainer::with_build(bool build) && {
    build_ = build;
    return std::move(*this);
}

DockerComposeContainer& DockerComposeContainer::with_pull(bool pull) & {
    pull_ = pull;
    return *this;
}
DockerComposeContainer&& DockerComposeContainer::with_pull(bool pull) && {
    pull_ = pull;
    return std::move(*this);
}

DockerComposeContainer& DockerComposeContainer::with_wait(bool wait) & {
    wait_ = wait;
    return *this;
}
DockerComposeContainer&& DockerComposeContainer::with_wait(bool wait) && {
    wait_ = wait;
    return std::move(*this);
}

DockerComposeContainer& DockerComposeContainer::with_wait_timeout(std::chrono::seconds timeout) & {
    wait_timeout_ = timeout;
    return *this;
}
DockerComposeContainer&&
DockerComposeContainer::with_wait_timeout(std::chrono::seconds timeout) && {
    wait_timeout_ = timeout;
    return std::move(*this);
}

DockerComposeContainer& DockerComposeContainer::with_remove_volumes(bool remove) & {
    remove_volumes_ = remove;
    return *this;
}
DockerComposeContainer&& DockerComposeContainer::with_remove_volumes(bool remove) && {
    remove_volumes_ = remove;
    return std::move(*this);
}

DockerComposeContainer& DockerComposeContainer::with_remove_images(bool remove) & {
    remove_images_ = remove;
    return *this;
}
DockerComposeContainer&& DockerComposeContainer::with_remove_images(bool remove) && {
    remove_images_ = remove;
    return std::move(*this);
}

void DockerComposeContainer::start() {
    // 1) Resolve the client (Local / Containerised / Auto). The containerised
    //    client starts its long-lived cli container here; keep it for teardown.
    client_ = compose::make_compose_client(to_internal_kind(client_kind_), compose_files_,
                                           compose_image_);

    // 2) Build and run the compose `up` command.
    compose::ComposeUpCommand up;
    up.project_name = project_;
    up.files = compose_files_; // the client overrides with its own paths
    for (const auto& [key, value] : env_) {
        up.env.emplace_back(key, value);
    }
    up.wait_timeout_secs = wait_timeout_.count();
    up.build = build_;
    up.pull = pull_;
    up.wait = wait_;
    client_->up(up); // throws DockerError (with output) on non-zero exit

    // 3) Discover the service containers by the compose project label.
    DockerClient docker = DockerClient::from_environment();
    service_to_id_.clear();
    const std::vector<ContainerSummary> summaries = docker.list_containers(
        {{"label", std::string(kComposeProjectLabel) + "=" + project_}}, /*all*/ true);
    for (const ContainerSummary& summary : summaries) {
        const auto it = summary.labels.find(kComposeServiceLabel);
        if (it == summary.labels.end()) {
            continue; // not a per-service container (skip)
        }
        service_to_id_.emplace(it->second, summary.id);
    }

    started_ = true;

    // 4) Extra guarantee on top of compose's `--wait`: wait for each exposed
    //    service's published host port to accept a TCP connection (mirrors the
    //    Reaper connect-retry loop). This confirms the port is actually open
    //    even for services without a healthcheck.
    for (const auto& [service, port] : exposed_services_) {
        const std::uint16_t host_port = get_service_port(service, port);
        const std::string host = get_service_host(service);

        asio::io_context io;
        boost::system::error_code ec;
        // The same user-configurable timeout that governs compose's --wait.
        const auto deadline = std::chrono::steady_clock::now() + wait_timeout_;
        bool connected = false;
        while (std::chrono::steady_clock::now() < deadline) {
            tcp::resolver resolver(io);
            const auto endpoints = resolver.resolve(host, std::to_string(host_port), ec);
            if (!ec) {
                tcp::socket socket(io);
                asio::connect(socket, endpoints, ec);
                if (!ec) {
                    connected = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!connected) {
            throw StartupTimeoutError("Compose service '" + service + "' host port " +
                                      std::to_string(host_port) +
                                      " did not accept a connection within the wait timeout: " +
                                      ec.message());
        }
    }
}

std::string DockerComposeContainer::get_service_host(const std::string& service) const {
    (void)get_service_container_id(service); // validate the service is known
    DockerClient client = DockerClient::from_environment();
    return client.host().http_host(); // "localhost" for a named pipe / unix socket
}

std::uint16_t DockerComposeContainer::get_service_port(const std::string& service,
                                                       ContainerPort port) const {
    const std::string id = get_service_container_id(service);
    DockerClient client = DockerClient::from_environment();
    const ContainerInspect info = client.inspect_container(id);
    return published_host_port(info, to_string(port));
}

std::string DockerComposeContainer::get_service_container_id(const std::string& service) const {
    const auto it = service_to_id_.find(service);
    if (it == service_to_id_.end()) {
        throw DockerError("Unknown compose service '" + service + "' in project '" + project_ +
                          "' (did start() run, and does the service publish a port?)");
    }
    return it->second;
}

void DockerComposeContainer::stop() { drop(); }

void DockerComposeContainer::drop() noexcept {
    // Always remove a temp `.yml` (from_yaml) regardless of started state.
    const auto delete_temp = [this]() noexcept {
        if (!temp_file_.empty()) {
            std::error_code ec;
            std::filesystem::remove(temp_file_, ec);
            temp_file_.clear();
        }
    };

    if (stopped_ || !started_) {
        stopped_ = true;
        client_.reset(); // releases the containerised cli container if any
        delete_temp();
        return;
    }
    stopped_ = true;
    try {
        // Best-effort: ask compose to tear the project down via the same client.
        if (client_) {
            try {
                compose::ComposeDownCommand down;
                down.project_name = project_;
                for (const auto& [key, value] : env_) {
                    down.env.emplace_back(key, value);
                }
                down.volumes = remove_volumes_;
                down.remove_images = remove_images_;
                client_->down(down);
            } catch (...) {
            }
        }

        // Best-effort: force-remove any container still carrying the project
        // label (e.g. if compose down missed one).
        try {
            DockerClient client = DockerClient::from_environment();
            const std::vector<ContainerSummary> leftovers = client.list_containers(
                {{"label", std::string(kComposeProjectLabel) + "=" + project_}}, /*all*/ true);
            for (const ContainerSummary& summary : leftovers) {
                try {
                    client.remove_container(summary.id, /*force*/ true, /*remove_volumes*/ true);
                } catch (...) {
                }
            }
        } catch (...) {
        }
    } catch (...) {
        // A teardown failure must never propagate (esp. from the destructor).
    }

    // Release the client (force-removes the containerised cli container) AFTER
    // `down`, then delete any temp file.
    client_.reset();
    delete_temp();
}

} // namespace testcontainers
