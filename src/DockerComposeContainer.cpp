#include "testcontainers/DockerComposeContainer.hpp"

#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <fstream>
#include <ios>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace testcontainers {

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

/// The ambassador image. Its entrypoint is `docker-compose`, so the container
/// command is just the compose arguments. Pinned to a known-good tag.
constexpr const char* kComposeImage = "docker/compose:1.29.2";

/// Where the single compose file is copied inside the ambassador container.
constexpr const char* kComposeFilePath = "/compose.yml";

constexpr const char* kManagedByLabel = "org.testcontainers.managed-by";

/// The label compose stamps on every container it creates for a project.
constexpr const char* kComposeProjectLabel = "com.docker.compose.project";
/// The label compose stamps with each container's service name.
constexpr const char* kComposeServiceLabel = "com.docker.compose.service";

/// Generate a random lowercase-hex id (a valid compose project name fragment),
/// matching Reaper's random_hex.
std::string random_hex(std::size_t chars) {
    static constexpr char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    std::string out;
    out.reserve(chars);
    for (std::size_t i = 0; i < chars; ++i) {
        out.push_back(hex[dist(rd)]);
    }
    return out;
}

/// Read the whole host file or throw (the compose file is read at construction).
std::string read_file_or_throw(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw DockerError("DockerComposeContainer: cannot read compose file '" + path + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Resolve the published host port for `key` (e.g. "6379/tcp") in an inspect,
/// preferring the IPv4 binding (mirrors Reaper / Container::get_host_port).
std::uint16_t published_host_port(const ContainerInspect& info, const std::string& key) {
    const auto it = info.ports.find(key);
    if (it == info.ports.end() || it->second.empty()) {
        throw DockerError("Compose service container " + info.id +
                          " published no host port for " + key);
    }
    std::uint16_t host_port = it->second.front().host_port;
    for (const PortBinding& binding : it->second) {
        if (binding.host_ip.find(':') == std::string::npos) { // IPv4 / empty host IP
            host_port = binding.host_port;
            break;
        }
    }
    return host_port;
}

/// Result of one ambassador run: its exit code and captured (stdout+stderr) logs.
struct ComposeRun {
    std::int64_t exit_code = 0;
    std::string logs;
};

/// Run the ambassador (`docker/compose` image) once with `args`, copying the
/// compose file in, and return its exit code + logs. The container is removed
/// before returning. Throws DockerError only on a Docker API failure (a non-zero
/// compose exit is reported via the returned exit code, not thrown).
ComposeRun run_compose(DockerClient& client, const std::string& image,
                       const std::string& compose_yaml,
                       const std::vector<std::string>& args) {
    CreateContainerSpec spec;
    spec.image = image;
    spec.cmd = args;
    // Compose talks to the daemon over the Linux docker socket — even on Windows
    // Docker Desktop the source is this in-VM path (NOT the named pipe), exactly
    // like the Ryuk reaper.
    spec.mounts = {Mount::bind("/var/run/docker.sock", "/var/run/docker.sock")};
    spec.working_dir = "/";
    // Mark it ours; do NOT auto-remove (we read the exit code and logs first).
    spec.labels = {{kManagedByLabel, "testcontainers"}};
    spec.auto_remove = false;

    const std::string id = client.create_container(spec);
    try {
        client.copy_to_container(id, CopyToContainer::content(compose_yaml, kComposeFilePath));
        client.start_container(id);

        // Poll until the ambassador exits (compose `up -d` / `down` are short).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        ContainerInspect info = client.inspect_container(id);
        while (info.running && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            info = client.inspect_container(id);
        }
        if (info.running) {
            throw DockerError("Compose ambassador did not exit within 120s");
        }

        ComposeRun run;
        run.exit_code = info.exit_code.value_or(-1);
        const ContainerLogs captured = client.logs(id);
        run.logs = captured.stdout_data + captured.stderr_data;

        client.remove_container(id, /*force*/ true, /*remove_volumes*/ true);
        return run;
    } catch (...) {
        try {
            client.remove_container(id, /*force*/ true, /*remove_volumes*/ true);
        } catch (...) {
        }
        throw;
    }
}

} // namespace

DockerComposeContainer::DockerComposeContainer(const std::string& compose_file_path)
    : compose_yaml_(read_file_or_throw(compose_file_path)), project_("tc" + random_hex(8)),
      compose_image_(kComposeImage) {}

DockerComposeContainer DockerComposeContainer::from_yaml(std::string compose_yaml) {
    DockerComposeContainer c;
    c.compose_yaml_ = std::move(compose_yaml);
    c.project_ = "tc" + random_hex(8);
    c.compose_image_ = kComposeImage;
    return c;
}

DockerComposeContainer::DockerComposeContainer(DockerComposeContainer&& other) noexcept
    : compose_yaml_(std::move(other.compose_yaml_)), project_(std::move(other.project_)),
      compose_image_(std::move(other.compose_image_)),
      exposed_services_(std::move(other.exposed_services_)),
      service_to_id_(std::move(other.service_to_id_)), started_(other.started_),
      stopped_(other.stopped_) {
    other.started_ = false;
    other.stopped_ = true; // the moved-from handle owns nothing — never tear down
}

DockerComposeContainer& DockerComposeContainer::operator=(DockerComposeContainer&& other) noexcept {
    if (this != &other) {
        drop();
        compose_yaml_ = std::move(other.compose_yaml_);
        project_ = std::move(other.project_);
        compose_image_ = std::move(other.compose_image_);
        exposed_services_ = std::move(other.exposed_services_);
        service_to_id_ = std::move(other.service_to_id_);
        started_ = other.started_;
        stopped_ = other.stopped_;
        other.started_ = false;
        other.stopped_ = true;
    }
    return *this;
}

DockerComposeContainer::~DockerComposeContainer() { drop(); }

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

void DockerComposeContainer::start() {
    DockerClient client = DockerClient::from_environment();

    // 1) Bring the stack up via the ambassador.
    const ComposeRun up =
        run_compose(client, compose_image_, compose_yaml_,
                    {"-f", kComposeFilePath, "-p", project_, "up", "-d"});
    if (up.exit_code != 0) {
        throw DockerError("Compose 'up' failed (exit " + std::to_string(up.exit_code) +
                          ") for project '" + project_ + "':\n" + up.logs);
    }

    // 2) Discover the service containers by the compose project label.
    service_to_id_.clear();
    const std::vector<ContainerSummary> summaries = client.list_containers(
        {{"label", std::string(kComposeProjectLabel) + "=" + project_}}, /*all*/ true);
    for (const ContainerSummary& summary : summaries) {
        const auto it = summary.labels.find(kComposeServiceLabel);
        if (it == summary.labels.end()) {
            continue; // not a per-service container (skip)
        }
        service_to_id_.emplace(it->second, summary.id);
    }

    started_ = true;

    // 3) Wait for each exposed service's published host port to accept a TCP
    //    connection (mirrors the Reaper connect-retry loop).
    for (const auto& [service, port] : exposed_services_) {
        const std::uint16_t host_port = get_service_port(service, port);
        const std::string host = get_service_host(service);

        asio::io_context io;
        boost::system::error_code ec;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
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
            throw DockerError("Compose service '" + service + "' host port " +
                              std::to_string(host_port) + " did not accept a connection: " +
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
    if (stopped_ || !started_) {
        stopped_ = true;
        return;
    }
    stopped_ = true;
    try {
        DockerClient client = DockerClient::from_environment();

        // Best-effort: ask compose to tear the project down (incl. its volumes).
        try {
            run_compose(client, compose_image_, compose_yaml_,
                        {"-f", kComposeFilePath, "-p", project_, "down", "-v"});
        } catch (...) {
        }

        // Best-effort: force-remove any container still carrying the project label
        // (e.g. if compose down missed one).
        try {
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
}

} // namespace testcontainers
