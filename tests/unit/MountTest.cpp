#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "testcontainers/Mount.hpp"

// Tests in this file:
//   Mount.BindFactory - bind sets Type=Bind with the host source and container target and defaults to read-write.
//   Mount.VolumeFactory - volume sets Type=Volume with the volume name as source and the container target.
//   Mount.TmpfsFactory - tmpfs sets Type=Tmpfs with the target, an empty source, and no tmpfs options.
//   Mount.ReadOnlySetter - with_read_only() flips the read-only flag and defaults to true.
//   Mount.TmpfsSizeAndModeSetters - with_tmpfs_size/with_tmpfs_mode populate the matching optionals.
//   Mount.SettersChainOnRvalue - the rvalue-qualified setters chain on a temporary.
//   Mount.Copyable - a Mount can be copied and the copy carries every field.

using namespace testcontainers;

TEST(Mount, BindFactory) {
    const Mount m = Mount::bind("/host/data", "/data");
    EXPECT_EQ(m.type(), MountType::Bind);
    EXPECT_EQ(m.source(), "/host/data");
    EXPECT_EQ(m.target(), "/data");
    EXPECT_FALSE(m.is_read_only());
    EXPECT_FALSE(m.tmpfs_size().has_value());
    EXPECT_FALSE(m.tmpfs_mode().has_value());
}

TEST(Mount, VolumeFactory) {
    const Mount m = Mount::volume("my-vol", "/var/lib/data");
    EXPECT_EQ(m.type(), MountType::Volume);
    EXPECT_EQ(m.source(), "my-vol");
    EXPECT_EQ(m.target(), "/var/lib/data");
}

TEST(Mount, TmpfsFactory) {
    const Mount m = Mount::tmpfs("/cache");
    EXPECT_EQ(m.type(), MountType::Tmpfs);
    EXPECT_TRUE(m.source().empty());
    EXPECT_EQ(m.target(), "/cache");
    EXPECT_FALSE(m.tmpfs_size().has_value());
    EXPECT_FALSE(m.tmpfs_mode().has_value());
}

TEST(Mount, ReadOnlySetter) {
    Mount m = Mount::bind("/host", "/data");
    m.with_read_only();
    EXPECT_TRUE(m.is_read_only());
    m.with_read_only(false);
    EXPECT_FALSE(m.is_read_only());
}

TEST(Mount, TmpfsSizeAndModeSetters) {
    Mount m = Mount::tmpfs("/cache");
    m.with_tmpfs_size(static_cast<std::int64_t>(64) * 1024 * 1024).with_tmpfs_mode(0700);

    ASSERT_TRUE(m.tmpfs_size().has_value());
    EXPECT_EQ(*m.tmpfs_size(), static_cast<std::int64_t>(64) * 1024 * 1024);
    ASSERT_TRUE(m.tmpfs_mode().has_value());
    EXPECT_EQ(*m.tmpfs_mode(), 0700);
}

TEST(Mount, SettersChainOnRvalue) {
    const Mount m = Mount::tmpfs("/cache").with_tmpfs_size(1024).with_read_only();
    EXPECT_TRUE(m.is_read_only());
    ASSERT_TRUE(m.tmpfs_size().has_value());
    EXPECT_EQ(*m.tmpfs_size(), 1024);
}

TEST(Mount, Copyable) {
    const Mount original = Mount::bind("/host", "/data").with_read_only();
    const Mount copy = original; // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(copy.type(), MountType::Bind);
    EXPECT_EQ(copy.source(), "/host");
    EXPECT_EQ(copy.target(), "/data");
    EXPECT_TRUE(copy.is_read_only());
}
