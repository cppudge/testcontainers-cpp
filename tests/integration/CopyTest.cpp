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

// Tests in this file (integration; require a Docker daemon):
//   Copy.CopyAtStartData - with_copy_to(content) lands the bytes in the container so a `cat` of the target prints them.
//   Copy.CopyAtStartHostFile - with_copy_to(host_file) copies a host file's contents into the container.
//   Copy.CopyIntoRunningContainer - Container::copy_to writes a file into an already-running container.
//   Copy.ReadFileRoundTrip - copy_to then read_file returns the exact bytes that were copied in.
//   Copy.LargeFileRoundTrip - a >1 MiB file copied in is read back byte-exact, proving the lifted response body limit.
//   Copy.CopyFileFromWritesHost - copy_file_from writes the container file's contents to a host path.
//   Copy.ReadFileRejectsDirectory - read_file on a directory throws DockerError (not a single regular file).

using namespace testcontainers;

// Requires a reachable Docker daemon; skipped if none is available.
class Copy : public ::testing::Test {
protected:
    void SetUp() override {
        if (auto why = tcit::linux_engine_unavailable()) {
            GTEST_SKIP() << *why;
        }
    }
};

namespace {

// A self-cleaning temp file holding `content`.
class TempFile {
public:
    explicit TempFile(const std::string& content) {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("tc_copytest_" + std::to_string(counter.fetch_add(1)));
        std::ofstream out(path_, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    std::string string() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

} // namespace

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
    const TempFile file(content);

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
    std::string big(2 * 1024 * 1024, 'x');
    big[big.size() - 3] = 'A';
    big[big.size() - 2] = 'B';
    big[big.size() - 1] = 'C';
    c.copy_to(CopyToContainer::content(big, "/tmp/big.bin"));

    const std::string read_back = c.read_file("/tmp/big.bin");
    ASSERT_EQ(read_back.size(), big.size());
    EXPECT_EQ(read_back, big);
}

TEST_F(Copy, CopyFileFromWritesHost) {
    Container c = GenericImage("alpine", "3.20").with_cmd({"sleep", "60"}).start();

    const std::string content = "to-the-host\n";
    c.copy_to(CopyToContainer::content(content, "/tmp/h.txt"));

    static std::atomic<unsigned> counter{0};
    const std::filesystem::path dest =
        std::filesystem::temp_directory_path() /
        ("tc_copyfrom_" + std::to_string(counter.fetch_add(1))) / "out.txt";

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
