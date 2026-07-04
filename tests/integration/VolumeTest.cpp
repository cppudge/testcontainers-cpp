#include <gtest/gtest.h>

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
//   Volumes.PopulateThenReadBack - populate() seeds a file into the volume via a helper container; a fresh container mounting the volume reads the seeded content back, proving it persisted in the volume.
//   WindowsVolumes.CreateInspectRemove - the same create/inspect/remove round-trip against a Windows daemon (the RAII variant is client-side logic and needs no per-engine mirror).
//   WindowsVolumes.DataPersistsAcrossContainers - a file written into a mounted volume from inside one container survives that container's removal and is read back by a fresh container mounting the same volume. (No populate() mirror: a Windows daemon extracts archive uploads into the container's layer, bypassing mounts — populate is Linux-only, see Volume.hpp.)

using namespace testcontainers;

// Requires a reachable Docker daemon; skipped if none is available. The fixture is
// named `Volumes` (plural) so the gtest suite does not collide with the
// `testcontainers::Volume` type (same convention as `Networks` in NetworkTest).
class Volumes : public ::testing::Test {
protected:
    void SetUp() override {
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

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
    EXPECT_EQ(res.exit_code, 0) << "stdout: " << res.stdout_data
                                << " stderr: " << res.stderr_data;
    EXPECT_NE(res.stdout_data.find("hello-volume"), std::string::npos)
        << "seeded data did not appear in the volume; stdout: " << res.stdout_data;

    // RAII tears down the container before the volume at scope exit (a volume in
    // use cannot be removed); the helper from populate() is already gone.
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
    // populate() cannot seed a Windows volume (archive uploads land in the
    // helper's layer, bypassing mounts — a daemon limitation shared by
    // `docker cp`), so seed the volume the way Windows users must: write from
    // INSIDE a container that mounts it. The write goes through the mount
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

        const ExecResult wr = writer.exec({"cmd", "/c", "echo hello-volume-win> C:\\data\\seed.txt"});
        ASSERT_EQ(wr.exit_code, 0) << "stdout: " << wr.stdout_data
                                   << " stderr: " << wr.stderr_data;
        // RAII removes the writer here; the volume (and the file) persist.
    }

    // A fresh container mounting the same volume sees the data.
    Container reader = nanoserver()
                           .with_cmd(keep_alive_cmd())
                           .with_user("ContainerAdministrator")
                           .with_mount(Mount::volume(v.name(), "C:/data"))
                           .start();

    const ExecResult res = reader.exec({"cmd", "/c", "type C:\\data\\seed.txt"});
    EXPECT_EQ(res.exit_code, 0) << "stdout: " << res.stdout_data
                                << " stderr: " << res.stderr_data;
    EXPECT_NE(res.stdout_data.find("hello-volume-win"), std::string::npos)
        << "seeded data did not appear in the volume; stdout: " << res.stdout_data;

    // RAII tears down the reader before the volume at scope exit (a volume in
    // use cannot be removed).
}
