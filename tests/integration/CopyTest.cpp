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
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/docker/DockerClient.hpp"

// Tests in this file (integration; require a Docker daemon):
//   Copy.CopyAtStartData - with_copy_to(content) lands the bytes in the container so a `cat` of the target prints them.
//   Copy.CopyAtStartHostFile - with_copy_to(host_file) copies a host file's contents into the container.
//   Copy.CopyIntoRunningContainer - Container::copy_to writes a file into an already-running container.

using namespace testcontainers;

// Requires a reachable Docker daemon; skipped if none is available.
class Copy : public ::testing::Test {
protected:
    void SetUp() override {
        try {
            DockerClient client = DockerClient::from_environment();
            if (!client.ping()) {
                GTEST_SKIP() << "Docker daemon did not respond to /_ping";
            }
        } catch (const std::exception& e) {
            GTEST_SKIP() << "Docker not available: " << e.what();
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
