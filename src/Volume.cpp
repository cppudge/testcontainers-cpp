#include "testcontainers/Volume.hpp"

#include <string>

#include "testcontainers/Mount.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"

#include "RandomHex.hpp"
#include "Reaper.hpp"

namespace testcontainers {

namespace {

/// Generate a unique-ish volume name like "tc-1a2b3c4d5e6f7a8b".
std::string random_volume_name() { return "tc-" + detail::random_hex(16); }

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

Volume Volume::create() { return create(random_volume_name()); }

Volume Volume::Builder::create() const {
    // Bring up the reaper first so a volume created here is reaped on a crash
    // (no-op if Ryuk is disabled).
    detail::Reaper::instance().ensure_started();

    VolumeCreateSpec spec;
    // A volume always needs a name; default to a generated unique one.
    spec.name = name_.empty() ? random_volume_name() : name_;
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
    // A throwaway helper with THIS volume mounted at `mount_path`; copying into
    // that path writes through to the volume, which persists when the helper is
    // removed. The helper carries the testcontainers labels so it is reaped if we
    // crash mid-seed. Linux daemons only: a Windows daemon extracts the archive
    // into the helper's layer, silently bypassing the mount (see the header).
    CreateContainerSpec spec;
    spec.image = helper_image;
    // We START the helper before copying. Empirically (see VolumeTest) copying to
    // a created-but-not-started helper also lands in the bind-mounted volume on
    // the dev daemon, but `PUT /containers/{id}/archive` is not guaranteed to
    // materialize on the volume's mountpoint until the container is running on
    // every daemon, so starting is the portable choice. `sleep` keeps it alive
    // long enough to copy, then we force-remove it (the volume persists).
    spec.cmd = {"sleep", "30"};
    spec.mounts = {Mount::volume(name_, mount_path)};
    spec.labels = detail::testcontainers_labels();

    const std::string helper_id = client_.create_container(spec);
    try {
        client_.start_container(helper_id);
        for (const CopyToContainer& source : sources) {
            // Rebase the (absolute) target under the mount path so it lands at that
            // path inside the volume (e.g. "/seed.txt" -> "<mount_path>/seed.txt").
            const std::string rebased_target = mount_path + source.target();
            CopyToContainer rebased =
                source.is_file()
                    ? CopyToContainer::host_file(source.host_path(), rebased_target)
                          .with_mode(source.mode())
                    : CopyToContainer::content(source.bytes(), rebased_target).with_mode(source.mode());
            client_.copy_to_container(helper_id, rebased);
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
