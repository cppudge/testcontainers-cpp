#include "testcontainers/DockerComposeContainer.hpp"

#include "RandomHex.hpp"
#include "Reaper.hpp"
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
    const auto host_port = docker::select_host_port(info.ports, key, docker::HostPortFamily::Any);
    if (!host_port) {
        throw DockerError("Compose service container " + info.id + " published no host port for " +
                          key);
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

// ===== ActiveStack: the running project as an RAII object ==================
//
// Everything teardown needs is SNAPSHOTTED here by start(), and the destructor
// IS the teardown (compose `down`, the best-effort label sweep, then the
// client release). That makes every DockerComposeContainer special member
// defaultable — rule of zero: a moved-from handle holds a null active_ and
// tears nothing down, move-assignment releases the target's own stack via
// unique_ptr, and a field added to either class cannot be silently dropped
// from a hand-written move (there is none).
struct DockerComposeContainer::ActiveStack {
    /// Owns the containerised cli container (if any); released LAST (members
    /// are destroyed in reverse order, after ~ActiveStack ran `down`).
    std::unique_ptr<compose::IComposeClient> client;
    std::string project;
    std::vector<std::pair<std::string, std::string>> env;
    bool remove_volumes = true;
    bool remove_images = false;
    /// compose service name -> discovered container id.
    std::map<std::string, std::string> service_to_id;

    ActiveStack() = default;
    ActiveStack(const ActiveStack&) = delete;
    ActiveStack& operator=(const ActiveStack&) = delete;

    /// Best-effort teardown; a failure must never propagate (this runs from
    /// destructors).
    ~ActiveStack() {
        try {
            // Ask compose to tear the project down via the same client.
            if (client) {
                try {
                    compose::ComposeDownCommand down;
                    down.project_name = project;
                    down.env = env;
                    down.volumes = remove_volumes;
                    down.remove_images = remove_images;
                    client->down(down);
                } catch (...) {
                    // Best-effort: compose down is advisory; the label sweep
                    // below catches what it missed.
                }
            }

            // Force-remove any container still carrying the project label
            // (e.g. if compose down missed one).
            try {
                DockerClient docker = DockerClient::from_environment();
                const std::vector<ContainerSummary> leftovers = docker.list_containers(
                    {{"label", std::string(kComposeProjectLabel) + "=" + project}}, /*all*/ true);
                for (const ContainerSummary& summary : leftovers) {
                    try {
                        docker.remove_container(summary.id, /*force*/ true,
                                                /*remove_volumes*/ true);
                    } catch (...) {
                        // Best-effort: keep sweeping the remaining leftovers.
                    }
                }
            } catch (...) {
                // Best-effort: the sweep itself must not break teardown.
            }
        } catch (...) {
            // Best-effort: teardown must never propagate.
        }
    }
};

// ===== TempFile =============================================================

DockerComposeContainer::TempFile::~TempFile() { remove(); }

DockerComposeContainer::TempFile&
DockerComposeContainer::TempFile::operator=(TempFile&& other) noexcept {
    if (this != &other) {
        remove();
        path_ = std::move(other.path_);
        other.path_.clear();
    }
    return *this;
}

void DockerComposeContainer::TempFile::remove() noexcept {
    if (!path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        path_.clear();
    }
}

// ============================================================================

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

DockerComposeContainer DockerComposeContainer::from_yaml(const std::string& compose_yaml) {
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
    c.temp_file_ = TempFile(temp.string());
    c.compose_files_ = {c.temp_file_.path()};
    return c;
}

// Rule of zero for the moves (see the ActiveStack note above): TempFile and
// unique_ptr carry all the transfer/release semantics, so no member list is
// hand-written. Defaulted out of line — ~unique_ptr<ActiveStack> needs the
// complete type. The destructor is stop(): member destruction alone would
// delete the temp file BEFORE the teardown that may re-read it (members are
// destroyed in reverse declaration order, and active_ is declared first so
// that the move-ASSIGN — which runs in declaration order — releases the
// target's stack before its temp file).
DockerComposeContainer::DockerComposeContainer(DockerComposeContainer&&) noexcept = default;
DockerComposeContainer&
DockerComposeContainer::operator=(DockerComposeContainer&&) noexcept = default;
DockerComposeContainer::~DockerComposeContainer() { stop(); }

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
    // 0) A restart must tear the previous run down BEFORE the new `up`: both
    //    runs share the project name/label, so tearing down afterwards (the
    //    unique_ptr-assignment order) would down + sweep the containers the
    //    new `up` just created. The temp file (if any) is still on disk here,
    //    so the old stack's `down -f` re-reads it fine.
    active_.reset();

    // 1) Build the stack-to-be: resolve the client (Local / Containerised /
    //    Auto — the containerised client starts its long-lived cli container
    //    here) and snapshot the teardown config. If anything below throws,
    //    ~ActiveStack already cleans up whatever came up — including a
    //    PARTIAL `up` (the old started_-flag scheme skipped teardown there).
    auto stack = std::make_unique<ActiveStack>();
    stack->client = compose::make_compose_client(to_internal_kind(client_kind_), compose_files_,
                                                 compose_image_);
    stack->project = project_;
    for (const auto& [key, value] : env_) {
        stack->env.emplace_back(key, value);
    }
    stack->remove_volumes = remove_volumes_;
    stack->remove_images = remove_images_;

    // Crash-safe reaping: the compose CLI creates the project's containers /
    // networks / volumes, so they carry compose's project label rather than
    // our session label — hand Ryuk an extra filter matching the project.
    // Registered BEFORE `up` so a crash mid-up is swept too; after a clean
    // stop() the filter simply matches nothing. No-op when Ryuk is disabled
    // (or skipped on a Windows-containers daemon); idempotent per project, so
    // a restart re-registers nothing.
    detail::Reaper::instance().register_filter(kComposeProjectLabel, project_);

    // 2) Build and run the compose `up` command.
    compose::ComposeUpCommand up;
    up.project_name = project_;
    up.files = compose_files_; // the client overrides with its own paths
    up.env = stack->env;
    up.wait_timeout_secs = wait_timeout_.count();
    up.build = build_;
    up.pull = pull_;
    up.wait = wait_;
    stack->client->up(up); // throws DockerError (with output) on non-zero exit

    // 3) Discover the service containers by the compose project label.
    DockerClient docker = DockerClient::from_environment();
    const std::vector<ContainerSummary> summaries = docker.list_containers(
        {{"label", std::string(kComposeProjectLabel) + "=" + project_}}, /*all*/ true);
    for (const ContainerSummary& summary : summaries) {
        const auto it = summary.labels.find(kComposeServiceLabel);
        if (it == summary.labels.end()) {
            continue; // not a per-service container (skip)
        }
        stack->service_to_id.emplace(it->second, summary.id);
    }

    // Adopt (active_ is null here — any previous stack was torn down up top).
    active_ = std::move(stack);

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
            throw StartupTimeoutError(
                "Compose service '" + service + "' host port " + std::to_string(host_port) +
                    " did not accept a connection within the wait timeout: " + ec.message(),
                service);
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
    if (active_) {
        const auto it = active_->service_to_id.find(service);
        if (it != active_->service_to_id.end()) {
            return it->second;
        }
    }
    throw DockerError("Unknown compose service '" + service + "' in project '" + project_ +
                      "' (did start() run, and does the service publish a port?)");
}

void DockerComposeContainer::stop() {
    active_.reset();     // ~ActiveStack runs the teardown (never throws)
    temp_file_.remove(); // AFTER teardown: `down` may re-read the compose file
}

} // namespace testcontainers
