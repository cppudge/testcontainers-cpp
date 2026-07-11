#include "testcontainers/Volume.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "testcontainers/Error.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"

#include "RandomHex.hpp"
#include "Reaper.hpp"
#include "docker/Auth.hpp" // substitute_image_name (hub prefix for the helpers)

namespace testcontainers {

namespace {

/// Rebase `source`'s absolute target under `prefix` (both POSIX-style — a
/// Windows daemon maps "/x" archive targets onto C:\), preserving the source
/// kind and mode.
CopyToContainer rebase_source(const CopyToContainer& source, const std::string& prefix) {
    const std::string rebased_target = prefix + source.target();
    CopyToContainer rebased = [&] {
        if (source.is_dir()) {
            return CopyToContainer::host_dir(source.host_path(), rebased_target);
        }
        if (source.is_file()) {
            return CopyToContainer::host_file(source.host_path(), rebased_target);
        }
        return CopyToContainer::content(source.bytes(), rebased_target);
    }();
    return rebased.with_mode(source.mode());
}

/// `path` with backslashes: cmd.exe tools read a leading '/' as a switch.
std::string backslashed(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    return path;
}

/// The layer path the Windows seed is staged into before the in-container
/// copy moves it onto the volume.
constexpr const char* kWindowsStageDir = "/__tc_stage";

/// The Windows seed path. A Windows daemon extracts archive uploads into the
/// container LAYER, never through mounts — so stage into the layer while the
/// helper is still CREATED (Hyper-V isolation, Docker Desktop's default,
/// additionally rejects archive writes to a RUNNING container with HTTP 500),
/// then start it and xcopy the stage through the mount junction onto the
/// volume. The helper deliberately runs the daemon's DEFAULT isolation, so
/// the staged copy-to-created path is exercised live under hyperv (Docker
/// Desktop, WindowsVolumes.PopulateSeedsVolume) — and a hyperv-default
/// daemon can still run the convenient mismatched default image.
void populate_windows(DockerClient& client, const std::string& volume_name,
                      const std::vector<CopyToContainer>& sources, const std::string& mount_path,
                      const std::string& helper_image) {
    CreateContainerSpec spec;
    spec.image = helper_image;
    // nanoserver's `sleep`: ping blocks cmd.exe for ~5 minutes, ample headroom
    // for one exec'd copy before the force-remove below.
    spec.cmd = {"cmd", "/c", "ping -n 300 127.0.0.1 >nul"};
    // The volume directory's ACL denies nanoserver's default low-privilege
    // ContainerUser (empirical; WindowsVolumes.DataPersistsAcrossContainers).
    spec.user = "ContainerAdministrator";
    spec.mounts = {Mount::volume(volume_name, mount_path)};
    spec.labels = detail::testcontainers_labels();

    const std::string helper_id = client.create_container(spec);
    try {
        std::vector<CopyToContainer> staged;
        staged.reserve(sources.size());
        for (const CopyToContainer& source : sources) {
            staged.push_back(rebase_source(source, kWindowsStageDir));
        }
        client.copy_to_container(helper_id, staged);
        client.start_container(helper_id);
        if (!sources.empty()) {
            // xcopy invoked DIRECTLY, one path per argv element: the daemon
            // escapes each element itself, whereas quotes embedded in a
            // `cmd /c` string arrive doubled and xcopy sees "Invalid path"
            // (empirical). /i answers the file-or-directory prompt.
            const ExecResult copied =
                client.exec(helper_id, {"xcopy", backslashed("C:" + std::string(kWindowsStageDir)),
                                        backslashed(mount_path), "/e", "/i", "/q", "/y"});
            if (copied.exit_code != 0) {
                throw DockerError("populate: in-container copy onto the volume failed (xcopy "
                                  "exit " +
                                  std::to_string(copied.exit_code) + "): " + copied.stdout_data +
                                  copied.stderr_data);
            }
        }
    } catch (...) {
        // Always remove the helper even if the seed failed, so we never leak it.
        try {
            client.remove_container(helper_id, /*force=*/true);
        } catch (...) {
            // Best-effort: the seed failure rethrown below is the root cause.
        }
        throw;
    }
    // The volume persists; the helper is disposable.
    client.remove_container(helper_id, /*force=*/true);
}

} // namespace

Volume Volume::create(std::string name) {
    // Bring up the reaper first so a volume created here is reaped on a crash
    // (no-op if Ryuk is disabled).
    detail::Reaper::instance().ensure_started();

    VolumeCreateSpec spec;
    spec.name = std::move(name);
    // Tag the volume so Ryuk (and tooling) can find it: managed-by + session.
    spec.labels = detail::testcontainers_labels();

    DockerClient client = DockerClient::from_environment();
    std::string created = client.create_volume(spec);
    return Volume(std::move(client), std::move(created));
}

Volume Volume::create() { return create(detail::random_resource_name()); }

Volume Volume::Builder::create() const {
    // Bring up the reaper first so a volume created here is reaped on a crash
    // (no-op if Ryuk is disabled).
    detail::Reaper::instance().ensure_started();

    VolumeCreateSpec spec;
    // A volume always needs a name; default to a generated unique one.
    spec.name = name_.empty() ? detail::random_resource_name() : name_;
    spec.driver = driver_;
    spec.driver_opts = driver_opts_;
    spec.labels = labels_;
    // Merge the testcontainers session labels on top of the user labels so the
    // volume is always reaped.
    for (const auto& label : detail::testcontainers_labels()) {
        spec.labels.push_back(label);
    }

    DockerClient client = DockerClient::from_environment();
    std::string created = client.create_volume(spec);
    return Volume(std::move(client), std::move(created));
}

void Volume::remove() { drop(); }

VolumeInspect Volume::inspect() const { return client_.inspect_volume(name_); }

void Volume::populate(const std::vector<CopyToContainer>& sources, const std::string& mount_path,
                      const std::string& helper_image) const {
    // Fail loud on a non-absolute target: the rebase below would silently
    // misplace it outside the mount (Linux) or outside the staging dir
    // (Windows) — a "C:/x" spelling is misplaced the same way, hence
    // POSIX-absolute on both daemons.
    for (const CopyToContainer& source : sources) {
        if (source.target().empty() || source.target().front() != '/') {
            throw DockerError(
                "populate: source target must be absolute POSIX-style ('/path'), got '" +
                source.target() + "'");
        }
    }

    // A throwaway helper with THIS volume mounted; seeding it writes through to
    // the volume, which persists when the helper is removed. The helper carries
    // the testcontainers labels so it is reaped if we crash mid-seed. Windows
    // daemons need the staged variant (archives never extract through mounts
    // there); empty mount_path/helper_image resolve per daemon OS.
    if (client_.server_os() == "windows") {
        populate_windows(client_, name_, sources, mount_path.empty() ? "C:/__tc_seed" : mount_path,
                         docker::substitute_image_name(
                             helper_image.empty() ? "mcr.microsoft.com/windows/nanoserver:ltsc2022"
                                                  : helper_image));
        return;
    }
    const std::string mount = mount_path.empty() ? "/__tc_seed" : mount_path;

    CreateContainerSpec spec;
    spec.image = docker::substitute_image_name(helper_image.empty() ? "alpine:3.20" : helper_image);
    // We START the helper before copying. Empirically (see VolumeTest) copying to
    // a created-but-not-started helper also lands in the bind-mounted volume on
    // the dev daemon, but `PUT /containers/{id}/archive` is not guaranteed to
    // materialize on the volume's mountpoint until the container is running on
    // every daemon, so starting is the portable choice. `sleep` keeps it alive
    // long enough to copy, then we force-remove it (the volume persists).
    spec.cmd = {"sleep", "30"};
    spec.mounts = {Mount::volume(name_, mount)};
    spec.labels = detail::testcontainers_labels();

    const std::string helper_id = client_.create_container(spec);
    try {
        client_.start_container(helper_id);
        for (const CopyToContainer& source : sources) {
            // Rebase the (absolute) target under the mount path so it lands at that
            // path inside the volume (e.g. "/seed.txt" -> "<mount>/seed.txt").
            client_.copy_to_container(helper_id, rebase_source(source, mount));
        }
    } catch (...) {
        // Always remove the helper even if a copy failed, so we never leak it.
        try {
            client_.remove_container(helper_id, /*force=*/true);
        } catch (...) {
            // Best-effort: the copy failure rethrown below is the root cause.
        }
        throw;
    }
    // The volume persists; the helper is disposable.
    client_.remove_container(helper_id, /*force=*/true);
}

void Volume::drop() noexcept {
    if (dropped_) {
        return;
    }
    dropped_ = true;
    try {
        client_.remove_volume(name_);
    } catch (...) {
        // Best-effort: a teardown failure must never propagate (esp. from the
        // destructor).
    }
}

} // namespace testcontainers
