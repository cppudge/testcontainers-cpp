#include <gtest/gtest.h>

#include <algorithm>
#include <exception>
#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/Volume.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"
#include "TempPaths.hpp"
#include "WindowsEngine.hpp"

// Tests in this file (integration; require a Docker daemon — Linux mode for the
// Volumes suite, Windows mode for the WindowsVolumes mirror):
//   Volumes.CreateInspectRemove - Volume::create makes a real volume whose inspect() reports a matching name, "local" driver, and non-empty mountpoint; remove() succeeds and a later inspect_volume on the name throws the typed NotFoundError.
//   Volumes.RaiiRemovesOnDrop - a Volume removes its backing volume at scope exit, so inspect_volume on the captured name throws NotFoundError afterward.
//   Volumes.BuilderSetsNameAndLabels - Volume::builder() name + labels land on the created volume (asserted via inspect()).
//   Volumes.ListVolumesFindsByLabel - list_volumes with a label filter returns exactly the matching volume (name/labels/mountpoint); the name filter finds it too (substring match daemon-side, exact match post-filtered by the caller).
//   Volumes.PruneRemovesUnusedByLabel - prune_volumes with a label filter (+ {"all","true"} for named volumes on API 1.42+) reports both unused volumes deleted and they are actually gone (inspect throws NotFoundError).
//   Volumes.PopulateThenReadBack - populate() seeds a file into the volume via a helper container; a fresh container mounting the volume reads the seeded content back, proving it persisted in the volume; a relative source target throws up front instead of being silently misplaced.
//   Volumes.PopulateDirSource - populate() with a host_dir source seeds a whole tree into the volume (nested file readable from a fresh container mounting it).
//   WindowsVolumes.CreateInspectRemove - the same create/inspect/remove round-trip against a Windows daemon (the RAII variant is client-side logic and needs no per-engine mirror).
//   WindowsVolumes.DataPersistsAcrossContainers - a file written into a mounted volume from inside one container survives that container's removal and is read back by a fresh container mounting the same volume (the manual seeding path populate() automates).
//   WindowsVolumes.PopulateSeedsVolume - populate() on a Windows daemon (stage into the created helper's layer, then in-container xcopy onto the volume) lands a file and a nested tree; a fresh container mounting the volume reads both back.

using namespace testcontainers;

// Requires a reachable Docker daemon; skipped if none is available. The fixture is
// named `Volumes` (plural) so the gtest suite does not collide with the
// `testcontainers::Volume` type (same convention as `Networks` in NetworkTest).
class Volumes : public tcit::LinuxEngineTest {};

TEST_F(Volumes, CreateInspectRemove) {
    Volume v = Volume::create();
    ASSERT_FALSE(v.name().empty());

    const VolumeInspect info = v.inspect();
    EXPECT_EQ(info.name, v.name());
    EXPECT_EQ(info.driver, "local");
    EXPECT_FALSE(info.mountpoint.empty());

    const std::string name = v.name();
    EXPECT_NO_THROW(v.remove());

    // The volume is gone: a fresh inspect of the removed name throws the TYPED
    // 404 (pinning NotFoundError end-to-end, not just "some exception").
    DockerClient client = DockerClient::from_environment();
    EXPECT_THROW(client.inspect_volume(name), NotFoundError);
}

TEST_F(Volumes, RaiiRemovesOnDrop) {
    std::string name; // captured before the handle drops
    {
        Volume v = Volume::create();
        name = v.name();
        ASSERT_FALSE(name.empty());
        // RAII removes the volume at scope exit.
    }

    DockerClient client = DockerClient::from_environment();
    EXPECT_THROW(client.inspect_volume(name), NotFoundError);
}

TEST_F(Volumes, BuilderSetsNameAndLabels) {
    // Random suffix: volume create is silently idempotent (an existing name
    // returns the OLD volume, labels unchanged), so a fixed name could inherit
    // a leftover from a crashed run and fail confusingly at the label assert.
    const std::string name = "tc-test-volume-built-" + tcit::random_suffix();
    Volume v = Volume::builder().with_name(name).with_label("tc-test-label", "yes").create();
    EXPECT_EQ(v.name(), name);

    const VolumeInspect info = v.inspect();
    const auto label = info.labels.find("tc-test-label");
    ASSERT_NE(label, info.labels.end()) << "builder label missing from inspect";
    EXPECT_EQ(label->second, "yes");

    // RAII removes the volume at scope exit.
}

TEST_F(Volumes, ListVolumesFindsByLabel) {
    // A unique label value so stale resources from earlier runs can't collide.
    const std::string marker = "list-it-" + tcit::random_suffix();
    Volume tagged = Volume::builder()
                        .with_name("tc-list-vol-" + tcit::random_suffix())
                        .with_label("tc-list-marker", marker)
                        .create();
    // An unmarked volume alive at the same time: the filter must EXCLUDE it.
    Volume other = Volume::create();

    DockerClient dc = DockerClient::from_environment();
    const auto by_label = dc.list_volumes({{"label", "tc-list-marker=" + marker}});
    ASSERT_EQ(by_label.size(), 1u);
    EXPECT_EQ(by_label[0].name, tagged.name());
    EXPECT_EQ(by_label[0].labels.at("tc-list-marker"), marker);
    EXPECT_FALSE(by_label[0].mountpoint.empty()); // list entries carry the full shape

    // The name filter matches substrings daemon-side; exact-name callers
    // post-filter.
    const auto by_name = dc.list_volumes({{"name", tagged.name()}});
    bool found = false;
    for (const auto& v : by_name) {
        found = found || v.name == tagged.name();
    }
    EXPECT_TRUE(found) << "name filter did not return the volume";

    // Both volumes are removed by RAII at scope exit.
}

TEST_F(Volumes, PruneRemovesUnusedByLabel) {
    // Client-made volumes without RAII handles: prune is the remover under test.
    const std::string marker = "prune-it-" + tcit::random_suffix();
    DockerClient dc = DockerClient::from_environment();

    const std::string name_a = "tc-prune-a-" + tcit::random_suffix();
    const std::string name_b = "tc-prune-b-" + tcit::random_suffix();

    // Armed from before the first create: raw create_volume carries no
    // session labels, so Ryuk would never sweep a leak — any failure between
    // here and the verified prune must remove whatever was created (removing
    // a never-created name just throws NotFoundError, swallowed).
    struct Cleanup {
        DockerClient& dc;
        const std::string& a;
        const std::string& b;
        bool armed = true;
        ~Cleanup() {
            if (!armed) {
                return;
            }
            try {
                dc.remove_volume(a, true);
            } catch (...) {
                // Best-effort: the test failure matters more.
            }
            try {
                dc.remove_volume(b, true);
            } catch (...) {
                // Best-effort: the test failure matters more.
            }
        }
    } cleanup{dc, name_a, name_b};

    VolumeCreateSpec spec;
    spec.name = name_a;
    spec.labels = {{"tc-prune-marker", marker}};
    ASSERT_EQ(dc.create_volume(spec), name_a);
    spec.name = name_b;
    ASSERT_EQ(dc.create_volume(spec), name_b);

    // The label filter narrows the sweep to just ours; named volumes also
    // need {"all","true"} on API 1.42+ daemons (anonymous-only default).
    const VolumePruneResult report =
        dc.prune_volumes({{"label", "tc-prune-marker=" + marker}, {"all", "true"}});

    // Both (unused) volumes are reported deleted — and are actually gone.
    EXPECT_EQ(std::count(report.deleted.begin(), report.deleted.end(), name_a), 1);
    EXPECT_EQ(std::count(report.deleted.begin(), report.deleted.end(), name_b), 1);
    EXPECT_THROW(dc.inspect_volume(name_a), NotFoundError);
    EXPECT_THROW(dc.inspect_volume(name_b), NotFoundError);

    // Only a fully verified prune disarms the guard: on any failed expectation
    // above, teardown still removes the leftovers.
    if (!::testing::Test::HasFailure()) {
        cleanup.armed = false;
    }
}

TEST_F(Volumes, PopulateThenReadBack) {
    Volume v = Volume::create();
    // Seed "/seed.txt" inside the volume (the helper mounts it and copies through).
    v.populate({CopyToContainer::content("hello-volume\n", "/seed.txt")});

    // Mount the (now-seeded) volume into a fresh container and read the file back.
    Container c = GenericImage::from_reference("alpine:3.20")
                      .with_cmd({"sleep", "30"})
                      .with_mount(Mount::volume(v.name(), "/data"))
                      .start();

    const ExecResult res = c.exec({"cat", "/data/seed.txt"});
    EXPECT_EQ(res.exit_code, 0) << "stdout: " << res.stdout_data << " stderr: " << res.stderr_data;

    // A relative target would be silently misplaced by the rebase — populate
    // refuses it up front (before any helper is created).
    EXPECT_THROW(v.populate({CopyToContainer::content("x", "relative.txt")}), DockerError);
    EXPECT_NE(res.stdout_data.find("hello-volume"), std::string::npos)
        << "seeded data did not appear in the volume; stdout: " << res.stdout_data;

    // RAII tears down the container before the volume at scope exit (a volume in
    // use cannot be removed); the helper from populate() is already gone.
}

TEST_F(Volumes, PopulateDirSource) {
    const tcit::TempTree tree;
    Volume v = Volume::create();
    // Seed a whole host tree at "/media" inside the volume.
    v.populate({CopyToContainer::host_dir(tree.path(), "/media")});

    Container c = GenericImage::from_reference("alpine:3.20")
                      .with_cmd({"sleep", "30"})
                      .with_mount(Mount::volume(v.name(), "/data"))
                      .start();

    const ExecResult res = c.exec({"cat", "/data/media/sub/nested.txt"});
    EXPECT_EQ(res.exit_code, 0) << "stdout: " << res.stdout_data << " stderr: " << res.stderr_data;
    EXPECT_NE(res.stdout_data.find("nested-body"), std::string::npos)
        << "seeded tree did not appear in the volume; stdout: " << res.stdout_data;
}

// The Windows mirror: the volume API itself is engine-agnostic, but the helper
// container, mount paths, and read-back tooling are all Windows-flavoured.
class WindowsVolumes : public tcit::WindowsEngineTest {};

TEST_F(WindowsVolumes, CreateInspectRemove) {
    Volume v = Volume::create();
    ASSERT_FALSE(v.name().empty());

    const VolumeInspect info = v.inspect();
    EXPECT_EQ(info.name, v.name());
    EXPECT_EQ(info.driver, "local");
    EXPECT_FALSE(info.mountpoint.empty());

    const std::string name = v.name();
    EXPECT_NO_THROW(v.remove());

    // The volume is gone: a fresh inspect of the removed name throws the TYPED
    // 404 — same NotFoundError contract as on the Linux engine.
    DockerClient client = DockerClient::from_environment();
    EXPECT_THROW(client.inspect_volume(name), NotFoundError);
}

TEST_F(WindowsVolumes, DataPersistsAcrossContainers) {
    // The manual seeding path (what populate() automates on Windows): archive
    // uploads land in the helper's layer, bypassing mounts — a daemon
    // limitation shared by `docker cp` — so a write into the volume must come
    // from INSIDE a container that mounts it. The write goes through the mount
    // junction and must outlive the writer. ContainerAdministrator throughout:
    // the volume directory's ACL does not grant nanoserver's default
    // low-privilege ContainerUser access.
    Volume v = Volume::create();
    {
        Container writer = nanoserver()
                               .with_cmd(keep_alive_cmd())
                               .with_user("ContainerAdministrator")
                               .with_mount(Mount::volume(v.name(), "C:/data"))
                               .start();

        const ExecResult wr =
            writer.exec({"cmd", "/c", "echo hello-volume-win> C:\\data\\seed.txt"});
        ASSERT_EQ(wr.exit_code, 0) << "stdout: " << wr.stdout_data << " stderr: " << wr.stderr_data;
        // RAII removes the writer here; the volume (and the file) persist.
    }

    // A fresh container mounting the same volume sees the data.
    Container reader = nanoserver()
                           .with_cmd(keep_alive_cmd())
                           .with_user("ContainerAdministrator")
                           .with_mount(Mount::volume(v.name(), "C:/data"))
                           .start();

    const ExecResult res = reader.exec({"cmd", "/c", "type C:\\data\\seed.txt"});
    EXPECT_EQ(res.exit_code, 0) << "stdout: " << res.stdout_data << " stderr: " << res.stderr_data;
    EXPECT_NE(res.stdout_data.find("hello-volume-win"), std::string::npos)
        << "seeded data did not appear in the volume; stdout: " << res.stdout_data;

    // RAII tears down the reader before the volume at scope exit (a volume in
    // use cannot be removed).
}

TEST_F(WindowsVolumes, PopulateSeedsVolume) {
    // populate() on a Windows daemon stages into the helper's LAYER while it
    // is still created-not-started, then xcopies onto the volume from inside
    // (archives never extract through mounts there — see Volume.hpp). The
    // helper image is build-matched here: a process-isolation daemon rejects
    // a mismatched build.
    Volume v = Volume::create();
    v.populate({CopyToContainer::content("hello-populate-win\n", "/seed.txt"),
                CopyToContainer::content("nested-win\n", "/sub/dir/nested.txt")},
               /*mount_path=*/{}, std::string(tcit::kWindowsImage) + ":" + tag_);

    // A fresh container mounting the volume reads both files back — the tree
    // itself landed on the volume, not the staging directory.
    Container reader = nanoserver()
                           .with_cmd(keep_alive_cmd())
                           .with_user("ContainerAdministrator")
                           .with_mount(Mount::volume(v.name(), "C:/data"))
                           .start();

    const ExecResult seed = reader.exec({"cmd", "/c", "type C:\\data\\seed.txt"});
    EXPECT_EQ(seed.exit_code, 0) << "stdout: " << seed.stdout_data
                                 << " stderr: " << seed.stderr_data;
    EXPECT_NE(seed.stdout_data.find("hello-populate-win"), std::string::npos) << seed.stdout_data;

    const ExecResult nested = reader.exec({"cmd", "/c", "type C:\\data\\sub\\dir\\nested.txt"});
    EXPECT_EQ(nested.exit_code, 0)
        << "stdout: " << nested.stdout_data << " stderr: " << nested.stderr_data;
    EXPECT_NE(nested.stdout_data.find("nested-win"), std::string::npos) << nested.stdout_data;

    // RAII tears down the reader before the volume at scope exit.
}
