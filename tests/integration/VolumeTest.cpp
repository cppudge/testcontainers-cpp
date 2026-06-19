#include <gtest/gtest.h>

#include <exception>
#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/Volume.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Docker daemon):
//   Volumes.CreateInspectRemove - Volume::create makes a real volume whose inspect() reports a matching name, "local" driver, and non-empty mountpoint; remove() succeeds and a later inspect_volume on the name throws (gone).
//   Volumes.RaiiRemovesOnDrop - a Volume removes its backing volume at scope exit, so inspect_volume on the captured name throws afterward.
//   Volumes.PopulateThenReadBack - populate() seeds a file into the volume via a helper container; a fresh container mounting the volume reads the seeded content back, proving it persisted in the volume.

using namespace testcontainers;

// Requires a reachable Docker daemon; skipped if none is available. The fixture is
// named `Volumes` (plural) so the gtest suite does not collide with the
// `testcontainers::Volume` type (same convention as `Networks` in NetworkTest).
class Volumes : public ::testing::Test {
protected:
    void SetUp() override {
        if (auto why = tcit::linux_engine_unavailable()) {
            GTEST_SKIP() << *why;
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

    // The volume is gone: a fresh inspect of the removed name throws (404).
    DockerClient client = DockerClient::from_environment();
    EXPECT_THROW(client.inspect_volume(name), std::exception);
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
    EXPECT_THROW(client.inspect_volume(name), std::exception);
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
