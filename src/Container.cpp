#include "testcontainers/Container.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include "docker/Ports.hpp"
#include "docker/Tar.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"

namespace testcontainers {

std::uint16_t Container::get_host_port(ContainerPort port) const {
    const ContainerInspect info = client_.inspect_container(id_);
    const std::string key = to_string(port); // e.g. "6379/tcp"
    const auto host_port = docker::select_host_port(info.ports, key, docker::HostPortFamily::Any);
    if (!host_port) {
        throw DockerError("Container " + id_ + " has no published host port for " + key,
                          std::nullopt, id_);
    }
    return *host_port;
}

std::uint16_t Container::get_host_port_ipv4(ContainerPort port) const {
    const ContainerInspect info = client_.inspect_container(id_);
    const std::string key = to_string(port);
    const auto host_port = docker::select_host_port(info.ports, key, docker::HostPortFamily::Ipv4);
    if (!host_port) {
        throw DockerError("Container " + id_ + " has no IPv4-published host port for " + key,
                          std::nullopt, id_);
    }
    return *host_port;
}

std::uint16_t Container::get_host_port_ipv6(ContainerPort port) const {
    const ContainerInspect info = client_.inspect_container(id_);
    const std::string key = to_string(port);
    const auto host_port = docker::select_host_port(info.ports, key, docker::HostPortFamily::Ipv6);
    if (!host_port) {
        throw DockerError("Container " + id_ + " has no IPv6-published host port for " + key,
                          std::nullopt, id_);
    }
    return *host_port;
}

std::uint16_t Container::first_mapped_port() const {
    // When we know the exposed-port order (set by GenericImage::start), the FIRST
    // declared port is the natural "the" port — resolve it directly.
    if (!exposed_ports_.empty()) {
        return get_host_port(exposed_ports_.front());
    }
    // Otherwise (an adopted / manually-constructed handle) fall back to the
    // lowest-numbered published container port.
    const ContainerInspect info = client_.inspect_container(id_);
    const auto host_port =
        docker::lowest_published_host_port(info.ports, docker::HostPortFamily::Any);
    if (!host_port) {
        throw DockerError("Container " + id_ + " publishes no ports", std::nullopt, id_);
    }
    return *host_port;
}

ContainerInspect Container::inspect() const { return client_.inspect_container(id_); }

std::string Container::inspect_raw() const { return client_.inspect_container_raw(id_); }

ContainerInspect Container::inspect(const std::string& id) {
    return DockerClient::from_environment().inspect_container(id);
}

ContainerLogs Container::logs() const {
    LogOptions opts;
    opts.tty = tty_; // a TTY container has a raw/unframed log stream (skip demux)
    return client_.logs(id_, opts);
}

void Container::follow_logs(const LogConsumer& consumer, const LogOptions& opts) const {
    LogOptions effective = opts;
    // Honor an explicit caller opt-in, and apply the remembered TTY flag so a
    // TTY container's raw stream is never demuxed.
    effective.tty = opts.tty || tty_;
    client_.follow_logs(id_, effective, consumer);
}

ExecResult Container::exec(const std::vector<std::string>& cmd) const {
    return client_.exec(id_, cmd);
}

ExecResult Container::exec(const std::vector<std::string>& cmd, const ExecOptions& opts) const {
    return client_.exec(id_, cmd, opts);
}

ExecResult Container::exec(const std::vector<std::string>& cmd, const ExecOptions& opts,
                           const LogConsumer& consumer) const {
    return client_.exec(id_, cmd, opts, consumer);
}

ExecStreamResult Container::exec(const std::vector<std::string>& cmd, const ExecOptions& opts,
                                 const LogConsumer& consumer,
                                 std::chrono::steady_clock::time_point deadline) const {
    return client_.exec(id_, cmd, opts, consumer, deadline);
}

void Container::copy_to(const CopyToContainer& source) const {
    client_.copy_to_container(id_, source);
}

std::string Container::read_file(const std::string& container_path) const {
    const std::string tar = client_.copy_from_container(id_, container_path);
    const std::vector<docker::TarEntry> entries = docker::extract_tar(tar);

    // The archive of a single file holds exactly one regular-file entry; a
    // directory holds many (and/or none). Require exactly one to read a file.
    const docker::TarEntry* file = nullptr;
    std::size_t regular_count = 0;
    for (const docker::TarEntry& e : entries) {
        if (e.is_regular_file) {
            ++regular_count;
            file = &e;
        }
    }
    if (regular_count != 1) {
        throw DockerError("read_file('" + container_path + "') on container " + id_ +
                          " expected exactly one regular file in the archive but found " +
                          std::to_string(regular_count) + " (is it a directory?)");
    }
    return file->body;
}

void Container::copy_file_from(const std::string& container_path,
                               const std::filesystem::path& host_dest) const {
    const std::string bytes = read_file(container_path);

    const std::filesystem::path parent = host_dest.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            throw DockerError("copy_file_from('" + container_path + "', '" + host_dest.string() +
                              "'): cannot create parent directory: " + ec.message());
        }
    }

    std::ofstream out(host_dest, std::ios::binary);
    if (!out) {
        throw DockerError("copy_file_from('" + container_path + "', '" + host_dest.string() +
                          "'): cannot open host file for writing");
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw DockerError("copy_file_from('" + container_path + "', '" + host_dest.string() +
                          "'): failed writing host file");
    }
}

void Container::stop() {
    // Explicit stop is a teardown point: fire the stopping hooks (once) before
    // the container is stopped.
    fire_stopping();
    client_.stop_container(id_);
}

bool Container::is_running() const { return client_.inspect_container(id_).running; }

void Container::remove() { drop(); }

void Container::fire_stopping() noexcept {
    if (stopping_fired_) {
        return;
    }
    stopping_fired_ = true;
    for (const LifecycleHook& hook : stopping_hooks_) {
        if (!hook) {
            continue;
        }
        try {
            hook(client_, id_);
        } catch (...) {
            // Best-effort: a stopping hook must never propagate (esp. from the
            // destructor's drop() path).
        }
    }
}

void Container::drop() noexcept {
    if (dropped_) {
        return;
    }
    dropped_ = true;
    if (!remove_on_drop_) {
        // A persistent (reusable) handle leaves the container running so a later
        // run can adopt it; the caller is responsible for removing it. Do NOT
        // fire the stopping hooks — the container is intentionally left running.
        return;
    }
    // We are about to remove the container: this is a teardown point, so fire
    // the stopping hooks (once) before removal.
    fire_stopping();
    try {
        client_.remove_container(id_, /*force*/ true, /*remove_volumes*/ true);
    } catch (...) {
        // Best-effort: a teardown failure must never propagate (esp. from the
        // destructor).
    }
}

} // namespace testcontainers
