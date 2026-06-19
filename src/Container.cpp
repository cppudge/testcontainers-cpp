#include "testcontainers/Container.hpp"

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include "docker/Tar.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"

namespace testcontainers {

std::uint16_t Container::get_host_port(ContainerPort port) const {
    const ContainerInspect info = client_.inspect_container(id_);
    const std::string key = to_string(port); // e.g. "6379/tcp"

    const auto it = info.ports.find(key);
    if (it == info.ports.end() || it->second.empty()) {
        throw DockerError("Container " + id_ + " has no published host port for " + key);
    }
    // Docker may publish different host ports for the IPv4 (0.0.0.0) and IPv6
    // (::) bindings of the same container port. Prefer the IPv4 binding so a
    // connection to a "localhost" that resolves to 127.0.0.1 reaches the right
    // port; fall back to the first binding if only IPv6 is published.
    for (const PortBinding& binding : it->second) {
        if (binding.host_ip.find(':') == std::string::npos) { // IPv4 (or empty host IP)
            return binding.host_port;
        }
    }
    return it->second.front().host_port;
}

ContainerLogs Container::logs() const { return client_.logs(id_); }

void Container::follow_logs(const LogConsumer& consumer, const LogOptions& opts) const {
    client_.follow_logs(id_, opts, consumer);
}

ExecResult Container::exec(const std::vector<std::string>& cmd) const {
    return client_.exec(id_, cmd);
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
                               const std::string& host_dest) const {
    const std::string bytes = read_file(container_path);

    const std::filesystem::path dest(host_dest);
    const std::filesystem::path parent = dest.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            throw DockerError("copy_file_from('" + container_path + "', '" + host_dest +
                              "'): cannot create parent directory: " + ec.message());
        }
    }

    std::ofstream out(dest, std::ios::binary);
    if (!out) {
        throw DockerError("copy_file_from('" + container_path + "', '" + host_dest +
                          "'): cannot open host file for writing");
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw DockerError("copy_file_from('" + container_path + "', '" + host_dest +
                          "'): failed writing host file");
    }
}

void Container::stop() { client_.stop_container(id_); }

bool Container::is_running() const { return client_.inspect_container(id_).running; }

void Container::remove() { drop(); }

void Container::drop() noexcept {
    if (dropped_) {
        return;
    }
    dropped_ = true;
    if (!remove_on_drop_) {
        // A persistent (reusable) handle leaves the container running so a later
        // run can adopt it; the caller is responsible for removing it.
        return;
    }
    try {
        client_.remove_container(id_, /*force*/ true, /*remove_volumes*/ true);
    } catch (...) {
        // Best-effort: a teardown failure must never propagate (esp. from the
        // destructor).
    }
}

} // namespace testcontainers
