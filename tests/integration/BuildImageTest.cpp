#include <gtest/gtest.h>

#include <string>

#include "testcontainers/Container.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/ImageFromDockerfile.hpp"
#include "testcontainers/WaitFor.hpp"

#include "EngineGuard.hpp"

// Tests in this file (integration; require a Linux Docker daemon):
//   BuildImage.BuildsAndRunsInlineDockerfile - an inline Dockerfile builds an image that, run via from_reference, prints the baked-in content.
//   BuildImage.BuildFailureThrows - a Dockerfile whose RUN exits non-zero makes build() throw DockerError.

using namespace testcontainers;

// Requires a reachable Linux Docker daemon; skipped if none is available.
class BuildImage : public ::testing::Test {
protected:
    void SetUp() override {
        if (auto why = tcit::linux_engine_unavailable()) {
            GTEST_SKIP() << *why;
        }
    }
};

TEST_F(BuildImage, BuildsAndRunsInlineDockerfile) {
    // Build an image that bakes a known string into a file, then runs `cat` on it.
    const std::string ref =
        ImageFromDockerfile::from_content("FROM alpine:3.20\n"
                                          "RUN echo built-content > /built.txt\n"
                                          "CMD [\"cat\",\"/built.txt\"]")
            .build();
    ASSERT_FALSE(ref.empty());

    // Run the built image to completion; its logs should carry the baked content.
    Container c = GenericImage::from_reference(ref).with_wait(wait_for::exit()).start();

    const ContainerLogs out = c.logs();
    EXPECT_NE(out.stdout_data.find("built-content"), std::string::npos)
        << "stdout was: " << out.stdout_data;
}

TEST_F(BuildImage, BuildFailureThrows) {
    // A RUN that exits non-zero fails the build; Docker returns HTTP 200 with the
    // error embedded in the stream, so build() must surface it as a DockerError.
    EXPECT_THROW(
        ImageFromDockerfile::from_content("FROM alpine:3.20\nRUN exit 3").build(),
        DockerError);
}
