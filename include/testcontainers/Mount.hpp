#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace testcontainers {

/// How a mount is sourced, mirroring Docker's `HostConfig.Mounts[].Type`.
enum class MountType {
    Bind,   ///< host path -> container path
    Volume, ///< named volume -> container path
    Tmpfs,  ///< in-memory filesystem at the container path (no source)
};

/// A single filesystem mount for a container, mapping to one entry of Docker's
/// create-body `HostConfig.Mounts` array.
///
/// Build one with the static factories, then tune it with the chainable,
/// ref-qualified setters (same style as `Healthcheck`/`GenericImage`).
///
/// A plain, copyable value type (no Boost/Asio leakage).
class Mount {
public:
    /// Bind-mount a host path at `container_target` (Type=bind).
    static Mount bind(std::string host_source, std::string container_target) {
        Mount m;
        m.type_ = MountType::Bind;
        m.source_ = std::move(host_source);
        m.target_ = std::move(container_target);
        return m;
    }

    /// Mount a named volume at `container_target` (Type=volume).
    static Mount volume(std::string volume_name, std::string container_target) {
        Mount m;
        m.type_ = MountType::Volume;
        m.source_ = std::move(volume_name);
        m.target_ = std::move(container_target);
        return m;
    }

    /// Mount an in-memory tmpfs at `container_target` (Type=tmpfs, no source).
    static Mount tmpfs(std::string container_target) {
        Mount m;
        m.type_ = MountType::Tmpfs;
        m.target_ = std::move(container_target);
        return m;
    }

    // --- In-place, ref-qualified setters ---

    Mount& read_only(bool ro = true) & {
        read_only_ = ro;
        return *this;
    }
    Mount&& read_only(bool ro = true) && {
        read_only_ = ro;
        return std::move(*this);
    }

    Mount& with_tmpfs_size(std::int64_t bytes) & {
        tmpfs_size_ = bytes;
        return *this;
    }
    Mount&& with_tmpfs_size(std::int64_t bytes) && {
        tmpfs_size_ = bytes;
        return std::move(*this);
    }

    Mount& with_tmpfs_mode(int mode) & {
        tmpfs_mode_ = mode;
        return *this;
    }
    Mount&& with_tmpfs_mode(int mode) && {
        tmpfs_mode_ = mode;
        return std::move(*this);
    }

    // --- Getters ---

    MountType type() const noexcept { return type_; }
    const std::string& source() const noexcept { return source_; }
    const std::string& target() const noexcept { return target_; }
    bool is_read_only() const noexcept { return read_only_; }
    std::optional<std::int64_t> tmpfs_size() const noexcept { return tmpfs_size_; }
    std::optional<int> tmpfs_mode() const noexcept { return tmpfs_mode_; }

private:
    MountType type_ = MountType::Bind;
    std::string source_;
    std::string target_;
    bool read_only_ = false;
    std::optional<std::int64_t> tmpfs_size_;
    std::optional<int> tmpfs_mode_;
};

} // namespace testcontainers
