#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers {

/// A RAII handle to a named Docker volume.
///
/// Move-only: it owns a real external resource and removes the volume on
/// destruction (best-effort, exceptions swallowed). Copying is deleted so the
/// removal happens exactly once. Mount it into a container with
/// `Mount::volume(v.name(), target)`, or seed it with `populate()`.
class Volume {
public:
    /// A copyable builder for a richer named volume: driver, driver options, and
    /// labels. Defined in std types only (no Boost/json leak through the header).
    ///
    /// The `with_*` setters mutate in place and return `*this` by reference; a
    /// single unqualified overload chains on both a named lvalue and a temporary.
    /// `create()` assembles the spec, runs the Reaper hook, and creates the volume.
    class Builder {
    public:
        Builder& with_name(std::string name) {
            name_ = std::move(name);
            return *this;
        }

        Builder& with_driver(std::string driver) {
            driver_ = std::move(driver);
            return *this;
        }

        /// Add a driver option (`DriverOpts`). Add several to set multiple options.
        Builder& with_driver_opt(std::string key, std::string value) {
            driver_opts_.emplace_back(std::move(key), std::move(value));
            return *this;
        }

        /// Add a volume label (`Labels`). The testcontainers session labels are
        /// always merged in by `create()` for Ryuk reaping.
        Builder& with_label(std::string key, std::string value) {
            labels_.emplace_back(std::move(key), std::move(value));
            return *this;
        }

        /// Create the volume from the configured options, returning the handle.
        Volume create() const;

    private:
        std::string name_;
        std::optional<std::string> driver_;
        std::vector<std::pair<std::string, std::string>> driver_opts_;
        std::vector<std::pair<std::string, std::string>> labels_;
    };

    /// Start a builder for a richer volume (driver, driver options, labels).
    static Builder builder() { return Builder{}; }

    Volume(const Volume&) = delete;
    Volume& operator=(const Volume&) = delete;

    Volume(Volume&& other) noexcept
        : client_(std::move(other.client_)), name_(std::move(other.name_)),
          dropped_(other.dropped_) {
        other.dropped_ = true; // the moved-from handle owns nothing
    }

    Volume& operator=(Volume&& other) noexcept {
        if (this != &other) {
            drop();
            client_ = std::move(other.client_);
            name_ = std::move(other.name_);
            dropped_ = other.dropped_;
            other.dropped_ = true;
        }
        return *this;
    }

    /// Removes the volume unless it was already explicitly removed or moved-from.
    /// Never throws.
    ~Volume() { drop(); }

    /// Create a volume with the given name.
    static Volume create(std::string name);

    /// Create a volume with a generated unique name (`tc-<random hex>`).
    static Volume create();

    /// The volume's name (used to mount it into a container).
    const std::string& name() const noexcept { return name_; }

    /// Explicitly remove the volume now. Idempotent; after this the destructor
    /// does nothing.
    void remove();

    /// `GET /volumes/{name}` — the volume's current driver, mountpoint, scope,
    /// labels, and options. Throws DockerError if the volume is gone.
    VolumeInspect inspect() const;

    /// Seed data into the volume by copying `sources` into it via a throwaway
    /// helper container that has this volume mounted at `mount_path`.
    ///
    /// Each source's target (absolute POSIX-style "/x", also toward Windows
    /// daemons — anything else throws) is rebased so it lands at that path
    /// *within the volume*: a source targeting "/seed.txt" is readable as
    /// "seed.txt" right under wherever the volume is mounted next. The helper
    /// carries the testcontainers session labels (so it is reaped if we crash
    /// mid-seed) and is always force-removed afterwards; the volume persists.
    ///
    /// Empty `mount_path` / `helper_image` resolve per daemon OS: Linux
    /// "/__tc_seed" + "alpine:3.20", Windows "C:/__tc_seed" +
    /// "mcr.microsoft.com/windows/nanoserver:ltsc2022".
    ///
    /// Linux daemons copy straight through the mount. A Windows daemon
    /// extracts archive uploads into the container's LAYER, never through
    /// mounts (`docker cp` shares the blind spot), so there the seed is
    /// staged into the created (not yet started) helper's layer and an
    /// in-container `xcopy` (run as ContainerAdministrator — the volume
    /// directory's ACL denies nanoserver's default user) moves it onto the
    /// volume; file-source MODES apply on Linux only. Windows helpers run
    /// under the daemon's default isolation — pass a build-matched
    /// `helper_image` (e.g. nanoserver:ltsc2025 on a 26100 host) where that
    /// default is process isolation.
    void populate(const std::vector<CopyToContainer>& sources, const std::string& mount_path = {},
                  const std::string& helper_image = {}) const;

private:
    Volume(DockerClient client, std::string name)
        : client_(std::move(client)), name_(std::move(name)) {}

    /// Best-effort remove, swallowing any error. Marks the handle dropped.
    void drop() noexcept;

    // Mutable for the same reason as Container/Network: the client is a stateless
    // host config that opens a fresh connection per call.
    mutable DockerClient client_;
    std::string name_;
    bool dropped_ = false;
};

} // namespace testcontainers
