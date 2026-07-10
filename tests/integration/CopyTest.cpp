#include <gtest/gtest.h>

#include <atomic>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>

#include "testcontainers/Container.hpp"
#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"
#include "TempPaths.hpp"
#include "WindowsEngine.hpp"

// Tests in this file (integration; require a Docker daemon — Linux mode for the
// Copy suite, Windows mode for the WindowsCopy mirror):
//   Copy.CopyAtStartData - with_copy_to(content) lands the bytes in the container so a `cat` of the target prints them.
//   Copy.CopyAtStartHostFile - with_copy_to(host_file) copies a host file's contents into the container.
//   Copy.CopyIntoRunningContainer - Container::copy_to writes a file into an already-running container.
//   Copy.ReadFileRoundTrip - copy_to then read_file returns the exact bytes that were copied in.
//   Copy.LargeFileRoundTrip - a >1 MiB file copied in is read back byte-exact, proving the lifted response body limit.
//   Copy.LargeHostFileStreams - a 5 MiB HOST FILE rides the lazy block-streamed upload path and reads back byte-exact.
//   Copy.BatchedCopyLandsAllSources - the batched copy_to_container overload lands every source in one PUT.
//   Copy.CopyFileFromWritesHost - copy_file_from writes the container file's contents to a host path.
//   Copy.ReadFileRejectsDirectory - read_file on a directory throws DockerError (not a single regular file).
//   Copy.ModeAppliedToCopiedFile - CopyToContainer::with_mode(0755) lands as the file's permission bits (stat -c %a -> 755).
//   Copy.CopyDirAtStart - with_copy_to(host_dir) lands the whole tree under a target whose chain did not pre-exist: nested file contents match and the empty subdirectory exists.
//   Copy.CopyDirIntoRunningContainer - Container::copy_to with a host_dir source copies the tree into an already-running container.
//   WindowsCopy.CopyAtStartData - the same copy-at-start round-trip into a Windows container (targets live at the C: root — extraction runs as the daemon, no parent-dir assumptions).
//   WindowsCopy.CopyAtStartHostFile - with_copy_to(host_file) into a Windows container.
//   WindowsCopy.CopyIntoRunningContainer - Container::copy_to against a running Windows container.
//   WindowsCopy.ReadFileRoundTrip - copy_to then read_file over the Windows daemon returns the exact bytes.
//   WindowsCopy.LargeFileRoundTrip - a 2 MiB round-trip against a Windows daemon (named-pipe transport on CI).
//   WindowsCopy.CopyFileFromWritesHost - copy_file_from a Windows container to a host path.
//   WindowsCopy.ReadFileRejectsDirectory - read_file on a (freshly mkdir'd, empty) directory throws DockerError.
//   WindowsCopy.CopyAtStartDriveTarget - a drive-rooted "C:\..." target is normalized (drive dropped, '/'-separated) and the file lands at that path.
//   WindowsCopy.CopyDirAtStart - with_copy_to(host_dir) with a "C:\..." target creates the directory chain and lands the tree (nested file readable, empty dir present).

using namespace testcontainers;

// Requires a reachable Docker daemon; skipped if none is available.
class Copy : public ::testing::Test {
protected:
    void SetUp() override {
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

TEST_F(Copy, CopyAtStartData) {
    // /tmp already exists in the image, so the relative entry "tmp/copied.txt"
    // extracts cleanly at the root.
    Container c = GenericImage("alpine", "3.20")
                      .with_copy_to(CopyToContainer::content("hello-copy", "/tmp/copied.txt"))
                      .with_cmd({"sleep", "60"})
                      .start();

    const ExecResult res = c.exec({"cat", "/tmp/copied.txt"});
    EXPECT_EQ(res.exit_code, 0) << "stderr: " << res.stderr_data;
    EXPECT_NE(res.stdout_data.find("hello-copy"), std::string::npos)
        << "stdout was: " << res.stdout_data;
}

TEST_F(Copy, CopyAtStartHostFile) {
    const std::string content = "contents-from-host-file";
    const tcit::TempFile file(content, "tc_copytest_");

    Container c = GenericImage("alpine", "3.20")
                      .with_copy_to(CopyToContainer::host_file(file.string(), "/tmp/fromfile.txt"))
                      .with_cmd({"sleep", "60"})
                      .start();

    const ExecResult res = c.exec({"cat", "/tmp/fromfile.txt"});
    EXPECT_EQ(res.exit_code, 0) << "stderr: " << res.stderr_data;
    EXPECT_NE(res.stdout_data.find(content), std::string::npos)
        << "stdout was: " << res.stdout_data;
}

TEST_F(Copy, CopyIntoRunningContainer) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    c.copy_to(CopyToContainer::content("runtime", "/tmp/rt.txt"));

    const ExecResult res = c.exec({"cat", "/tmp/rt.txt"});
    EXPECT_EQ(res.exit_code, 0) << "stderr: " << res.stderr_data;
    EXPECT_NE(res.stdout_data.find("runtime"), std::string::npos)
        << "stdout was: " << res.stdout_data;
}

TEST_F(Copy, ReadFileRoundTrip) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    const std::string original = "round-trip-bytes\nwith two lines\n";
    c.copy_to(CopyToContainer::content(original, "/tmp/rt.txt"));

    const std::string read_back = c.read_file("/tmp/rt.txt");
    EXPECT_EQ(read_back, original);
}

TEST_F(Copy, LargeFileRoundTrip) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    // 2 MiB — comfortably over Beast's default 1 MiB body limit, so this only
    // passes because request() disables that limit.
    std::string big(std::size_t{2} * 1024 * 1024, 'x');
    big[big.size() - 3] = 'A';
    big[big.size() - 2] = 'B';
    big[big.size() - 1] = 'C';
    c.copy_to(CopyToContainer::content(big, "/tmp/big.bin"));

    const std::string read_back = c.read_file("/tmp/big.bin");
    ASSERT_EQ(read_back.size(), big.size());
    EXPECT_EQ(read_back, big);
}

TEST_F(Copy, LargeHostFileStreams) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    // A 5 MiB host file: the upload reads it in 64 KiB blocks as the chunks
    // go out (the lazy path — distinct from the in-memory `content` source).
    std::string big;
    big.reserve(std::size_t{5} * 1024 * 1024);
    for (std::size_t i = 0; big.size() < std::size_t{5} * 1024 * 1024; ++i) {
        big += "streamed host file payload line " + std::to_string(i) + "\n";
    }
    const tcit::TempFile file(big, "tc_copybig_");

    c.copy_to(CopyToContainer::host_file(file.string(), "/tmp/big-host.bin"));

    const std::string read_back = c.read_file("/tmp/big-host.bin");
    ASSERT_EQ(read_back.size(), big.size());
    EXPECT_EQ(read_back, big);
}

TEST_F(Copy, BatchedCopyLandsAllSources) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    const tcit::TempFile file("batched-host-file", "tc_copybatch_");
    const std::vector<CopyToContainer> sources = {
        CopyToContainer::content("batched-bytes", "/tmp/batch-a.txt"),
        CopyToContainer::host_file(file.string(), "/tmp/batch-b.txt"),
    };
    DockerClient::from_environment().copy_to_container(c.id(), sources);

    EXPECT_EQ(c.read_file("/tmp/batch-a.txt"), "batched-bytes");
    EXPECT_EQ(c.read_file("/tmp/batch-b.txt"), "batched-host-file");
}

TEST_F(Copy, CopyFileFromWritesHost) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    const std::string content = "to-the-host\n";
    c.copy_to(CopyToContainer::content(content, "/tmp/h.txt"));

    static std::atomic<unsigned> counter{0};
    const std::filesystem::path dest = std::filesystem::temp_directory_path() /
                                       ("tc_copyfrom_" + std::to_string(counter.fetch_add(1))) /
                                       "out.txt";

    c.copy_file_from("/tmp/h.txt", dest.string());

    std::ifstream in(dest, std::ios::binary);
    ASSERT_TRUE(in.good());
    const std::string on_host((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    EXPECT_EQ(on_host, content);

    std::error_code ec;
    std::filesystem::remove_all(dest.parent_path(), ec);
}

TEST_F(Copy, ReadFileRejectsDirectory) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    // /tmp is a directory: its archive has no single regular file, so read_file
    // must reject it.
    EXPECT_THROW(c.read_file("/tmp"), DockerError);
}

TEST_F(Copy, ModeAppliedToCopiedFile) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    c.copy_to(CopyToContainer::content("#!/bin/sh\n", "/tmp/m.sh").with_mode(0755));

    const ExecResult res = c.exec({"stat", "-c", "%a", "/tmp/m.sh"});
    EXPECT_EQ(res.exit_code, 0) << "stderr: " << res.stderr_data;
    EXPECT_NE(res.stdout_data.find("755"), std::string::npos) << "mode was: " << res.stdout_data;
}

TEST_F(Copy, CopyDirAtStart) {
    const tcit::TempTree tree;

    // "/opt/data/deep" does not exist in alpine: the dir source must create the
    // whole target chain itself.
    Container c = GenericImage("alpine", "3.20")
                      .with_copy_to(CopyToContainer::host_dir(tree.path(), "/opt/data/deep"))
                      .with_cmd({"sleep", "60"})
                      .start();

    const ExecResult root = c.exec({"cat", "/opt/data/deep/root.txt"});
    EXPECT_EQ(root.exit_code, 0) << "stderr: " << root.stderr_data;
    EXPECT_NE(root.stdout_data.find("root-body"), std::string::npos)
        << "stdout was: " << root.stdout_data;

    const ExecResult nested = c.exec({"cat", "/opt/data/deep/sub/nested.txt"});
    EXPECT_EQ(nested.exit_code, 0) << "stderr: " << nested.stderr_data;
    EXPECT_NE(nested.stdout_data.find("nested-body"), std::string::npos)
        << "stdout was: " << nested.stdout_data;

    // Empty directories must survive the copy.
    const ExecResult empty = c.exec({"test", "-d", "/opt/data/deep/empty"});
    EXPECT_EQ(empty.exit_code, 0) << "empty/ was not preserved";
}

TEST_F(Copy, CopyDirIntoRunningContainer) {
    const tcit::TempTree tree;
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    c.copy_to(CopyToContainer::host_dir(tree.path(), "/rt-tree"));

    const ExecResult res = c.exec({"cat", "/rt-tree/sub/nested.txt"});
    EXPECT_EQ(res.exit_code, 0) << "stderr: " << res.stderr_data;
    EXPECT_NE(res.stdout_data.find("nested-body"), std::string::npos)
        << "stdout was: " << res.stdout_data;
}

// The Windows mirror. All targets live at the filesystem root ("/x.txt" is
// C:\x.txt to the daemon): the copy endpoint extracts as the daemon, not the
// container user, so no writable-parent-directory assumptions are needed and
// no directory has to pre-exist in nanoserver.
class WindowsCopy : public tcit::WindowsEngineTest {
protected:
    /// A running nanoserver container to copy into/out of.
    testcontainers::Container start_keep_alive() {
        return nanoserver().with_cmd(keep_alive_cmd()).start();
    }
};

TEST_F(WindowsCopy, CopyAtStartData) {
    Container c = nanoserver()
                      .with_copy_to(CopyToContainer::content("hello-copy-win", "/copied.txt"))
                      .with_cmd(keep_alive_cmd())
                      .start();

    const ExecResult res = c.exec({"cmd", "/c", "type C:\\copied.txt"});
    EXPECT_EQ(res.exit_code, 0) << "stderr: " << res.stderr_data;
    EXPECT_NE(res.stdout_data.find("hello-copy-win"), std::string::npos)
        << "stdout was: " << res.stdout_data;
}

TEST_F(WindowsCopy, CopyAtStartHostFile) {
    const std::string content = "contents-from-host-file-win";
    const tcit::TempFile file(content, "tc_copytest_");

    Container c = nanoserver()
                      .with_copy_to(CopyToContainer::host_file(file.string(), "/fromfile.txt"))
                      .with_cmd(keep_alive_cmd())
                      .start();

    const ExecResult res = c.exec({"cmd", "/c", "type C:\\fromfile.txt"});
    EXPECT_EQ(res.exit_code, 0) << "stderr: " << res.stderr_data;
    EXPECT_NE(res.stdout_data.find(content), std::string::npos)
        << "stdout was: " << res.stdout_data;
}

TEST_F(WindowsCopy, CopyIntoRunningContainer) {
    Container c = start_keep_alive();

    c.copy_to(CopyToContainer::content("runtime-win", "/rt.txt"));

    const ExecResult res = c.exec({"cmd", "/c", "type C:\\rt.txt"});
    EXPECT_EQ(res.exit_code, 0) << "stderr: " << res.stderr_data;
    EXPECT_NE(res.stdout_data.find("runtime-win"), std::string::npos)
        << "stdout was: " << res.stdout_data;
}

TEST_F(WindowsCopy, ReadFileRoundTrip) {
    Container c = start_keep_alive();

    // CRLF on purpose: the bytes must survive verbatim, no newline translation.
    const std::string original = "round-trip-bytes\r\nwith two lines\r\n";
    c.copy_to(CopyToContainer::content(original, "/rt.txt"));

    const std::string read_back = c.read_file("/rt.txt");
    EXPECT_EQ(read_back, original);
}

TEST_F(WindowsCopy, LargeFileRoundTrip) {
    Container c = start_keep_alive();

    // 2 MiB — comfortably over Beast's default 1 MiB body limit; on CI this
    // also rides the named-pipe transport rather than a socket.
    std::string big(std::size_t{2} * 1024 * 1024, 'x');
    big[big.size() - 3] = 'A';
    big[big.size() - 2] = 'B';
    big[big.size() - 1] = 'C';
    c.copy_to(CopyToContainer::content(big, "/big.bin"));

    const std::string read_back = c.read_file("/big.bin");
    ASSERT_EQ(read_back.size(), big.size());
    EXPECT_EQ(read_back, big);
}

TEST_F(WindowsCopy, CopyFileFromWritesHost) {
    Container c = start_keep_alive();

    const std::string content = "to-the-host-win\r\n";
    c.copy_to(CopyToContainer::content(content, "/h.txt"));

    static std::atomic<unsigned> counter{0};
    const std::filesystem::path dest = std::filesystem::temp_directory_path() /
                                       ("tc_copyfrom_win_" + std::to_string(counter.fetch_add(1))) /
                                       "out.txt";

    c.copy_file_from("/h.txt", dest.string());

    std::ifstream in(dest, std::ios::binary);
    ASSERT_TRUE(in.good());
    const std::string on_host((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    EXPECT_EQ(on_host, content);

    std::error_code ec;
    std::filesystem::remove_all(dest.parent_path(), ec);
}

TEST_F(WindowsCopy, ReadFileRejectsDirectory) {
    Container c = start_keep_alive();

    // A freshly created, guaranteed-empty directory: its archive has no single
    // regular file, so read_file must reject it. (No pre-existing nanoserver
    // directory is used — one stray desktop.ini would turn this into a pass.)
    const ExecResult mk = c.exec({"cmd", "/c", "mkdir C:\\tc-empty"});
    ASSERT_EQ(mk.exit_code, 0) << "stderr: " << mk.stderr_data;

    EXPECT_THROW(c.read_file("/tc-empty"), DockerError);
}

TEST_F(WindowsCopy, CopyAtStartDriveTarget) {
    // The natural Windows spelling of a target: drive-rooted with backslashes.
    // Root-level, like every other file target here (single-file sources make
    // no parent-directory assumptions about nanoserver).
    Container c = nanoserver()
                      .with_copy_to(CopyToContainer::content("via-drive", "C:\\drive-target.txt"))
                      .with_cmd(keep_alive_cmd())
                      .start();

    const ExecResult res = c.exec({"cmd", "/c", "type C:\\drive-target.txt"});
    EXPECT_EQ(res.exit_code, 0) << "stderr: " << res.stderr_data;
    EXPECT_NE(res.stdout_data.find("via-drive"), std::string::npos)
        << "stdout was: " << res.stdout_data;
}

TEST_F(WindowsCopy, CopyDirAtStart) {
    const tcit::TempTree tree;

    // C:\tcdata does not exist in nanoserver: the dir source must create it.
    Container c = nanoserver()
                      .with_copy_to(CopyToContainer::host_dir(tree.path(), "C:\\tcdata"))
                      .with_cmd(keep_alive_cmd())
                      .start();

    const ExecResult nested = c.exec({"cmd", "/c", "type C:\\tcdata\\sub\\nested.txt"});
    EXPECT_EQ(nested.exit_code, 0) << "stderr: " << nested.stderr_data;
    EXPECT_NE(nested.stdout_data.find("nested-body"), std::string::npos)
        << "stdout was: " << nested.stdout_data;

    // `dir` exits 0 for an existing (even empty) directory, 1 otherwise.
    const ExecResult empty = c.exec({"cmd", "/c", "dir C:\\tcdata\\empty"});
    EXPECT_EQ(empty.exit_code, 0) << "empty/ was not preserved";
}
