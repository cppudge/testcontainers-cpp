#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "Reaper.hpp"
#include "testcontainers/Container.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/GenericBuildableImage.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include "EngineGuard.hpp"
#include "TempPaths.hpp"
#include "WindowsEngine.hpp"

// Tests in this file (integration; require a Docker daemon — Linux mode for the
// BuildImage suite, Windows mode for the WindowsBuildImage mirror):
//   BuildImage.BuildsAndRunsInlineDockerfile - an inline Dockerfile builds an image whose returned GenericImage, run to exit, prints the baked-in content.
//   BuildImage.BuildFailureThrows - a Dockerfile whose RUN exits non-zero makes build() throw DockerError.
//   BuildImage.ContextFilesBuildArgsNoCache - with_data + with_file land in the build context (COPY works), with_build_arg reaches RUN, with_no_cache still builds.
//   BuildImage.DockerfilePathAndTargetStage - with_dockerfile(host path) + with_target build only the named multi-stage target; with_pull refreshes the base.
//   BuildImage.BuildLogConsumerStreamsSteps - with_build_log_consumer receives the step banners and a RUN step's own echo output during build().
//   BuildImage.BuildFailureCarriesStepOutput - a failing RUN's own output (echoed before the exit) appears in the DockerError message, not just the daemon's exit-code line.
//   BuildImage.DockerignoreFiltersContext - a .dockerignore at a directory source's root keeps excluded files out of the uploaded context (COPY sees keep.txt and the .dockerignore itself, not *.log or the ignored dir).
//   BuildImage.BuiltImageCarriesSessionLabels - a built image carries the managed-by/session labels (so Ryuk reaps it), merged with the Dockerfile's own LABELs (query label wins a duplicate key), and a Ryuk sidecar is up after a pure build().
//   BuildImage.ExistsReflectsLocalImages - GenericImage::exists is true for a just-built tag and false for a name that was never built.
//   BuildImage.InspectReflectsImageConfig - GenericImage::inspect (static and instance) returns the built image's id/tag/os and Config (label, exposed port, workdir, cmd); inspect_image_raw returns the raw body; a never-built reference throws NotFoundError.
//   WindowsBuildImage.BuildsAndRunsInlineDockerfile - the same round-trip on a Windows daemon: a nanoserver-based Dockerfile bakes a file, the built image types it out.
//   WindowsBuildImage.BuildFailureThrows - a failing RUN (cmd `exit 3`) in a Windows build surfaces as DockerError.
//   WindowsBuildImage.ExistsAndBuildLogConsumer - on a Windows daemon: the consumer sees build output, GenericImage::exists reflects the built tag, and GenericImage::inspect reports os "windows" with the tag in repo_tags.

using namespace testcontainers;

// Requires a reachable Linux Docker daemon; skipped if none is available.
class BuildImage : public ::testing::Test {
protected:
    void SetUp() override {
        if (tcit::linux_engine_unavailable()) {
            GTEST_SKIP(); // no daemon / wrong engine mode; reason not streamed (CI noise)
        }
    }
};

TEST_F(BuildImage, BuildsAndRunsInlineDockerfile) {
    // Build an image that bakes a known string into a file, then runs `cat` on it.
    // build() returns a runnable GenericImage tagged "<name>:<tag>".
    GenericImage image = GenericBuildableImage("tc-build-test", "latest")
                             .with_dockerfile_string("FROM alpine:3.20\n"
                                                     "RUN echo built-content > /built.txt\n"
                                                     "CMD [\"cat\",\"/built.txt\"]")
                             .build();

    // Run the built image to completion; its logs should carry the baked content.
    Container c = image.with_wait(wait_for::exit()).start();

    const ContainerLogs out = c.logs();
    EXPECT_NE(out.stdout_data.find("built-content"), std::string::npos)
        << "stdout was: " << out.stdout_data;
}

TEST_F(BuildImage, BuildFailureThrows) {
    // A RUN that exits non-zero fails the build; Docker returns HTTP 200 with the
    // error embedded in the stream, so build() must surface it as a DockerError.
    EXPECT_THROW(GenericBuildableImage("tc-build-fail", "latest")
                     .with_dockerfile_string("FROM alpine:3.20\nRUN exit 3")
                     .build(),
                 DockerError);
}

TEST_F(BuildImage, ContextFilesBuildArgsNoCache) {
    const tcit::TempFile host_file("data-from-host-file\n", "tc_buildtest_");

    // Two context sources (in-memory + host file) COPY'd in, a build arg baked
    // into a file by RUN, and no-cache so the RUN really executes this build.
    GenericImage image =
        GenericBuildableImage("tc-build-ctx", "latest")
            .with_dockerfile_string("FROM alpine:3.20\n"
                                    "ARG MSG=unset\n"
                                    "COPY ctx.txt /ctx.txt\n"
                                    "COPY hostfile.txt /hostfile.txt\n"
                                    "RUN printf %s \"$MSG\" > /msg.txt\n"
                                    "CMD [\"sh\", \"-c\", \"cat /ctx.txt /hostfile.txt /msg.txt\"]")
            .with_data("data-from-memory\n", "ctx.txt")
            .with_file(host_file.path(), "hostfile.txt")
            .with_build_arg("MSG", "msg-from-arg")
            .with_no_cache()
            .build();

    Container c = image.with_wait(wait_for::exit()).start();

    const ContainerLogs out = c.logs();
    EXPECT_NE(out.stdout_data.find("data-from-memory"), std::string::npos)
        << "stdout was: " << out.stdout_data;
    EXPECT_NE(out.stdout_data.find("data-from-host-file"), std::string::npos)
        << "stdout was: " << out.stdout_data;
    EXPECT_NE(out.stdout_data.find("msg-from-arg"), std::string::npos)
        << "stdout was: " << out.stdout_data;
}

TEST_F(BuildImage, DockerfilePathAndTargetStage) {
    // The Dockerfile comes from a HOST file (with_dockerfile), and the build
    // stops at the "base" stage (with_target) — the "extra" stage would
    // overwrite the marker, so seeing "stage-base" proves the target applied.
    // with_pull additionally refreshes the base image from the registry.
    const tcit::TempFile dockerfile("FROM alpine:3.20 AS base\n"
                                    "RUN echo stage-base > /stage.txt\n"
                                    "CMD [\"cat\", \"/stage.txt\"]\n"
                                    "FROM base AS extra\n"
                                    "RUN echo stage-extra > /stage.txt\n");

    GenericImage image = GenericBuildableImage("tc-build-target", "latest")
                             .with_dockerfile(dockerfile.path())
                             .with_target("base")
                             .with_pull()
                             .build();

    Container c = image.with_wait(wait_for::exit()).start();

    const ContainerLogs out = c.logs();
    EXPECT_NE(out.stdout_data.find("stage-base"), std::string::npos)
        << "stdout was: " << out.stdout_data;
    EXPECT_EQ(out.stdout_data.find("stage-extra"), std::string::npos)
        << "the extra stage must not have built; stdout was: " << out.stdout_data;
}

TEST_F(BuildImage, BuildLogConsumerStreamsSteps) {
    // no_cache forces the RUN to really execute, so its echo output is in the
    // stream (a cache hit would replace it with "Using cache").
    std::string log;
    GenericBuildableImage("tc-build-consumer", "latest")
        .with_dockerfile_string("FROM alpine:3.20\n"
                                "RUN echo consumer-sees-this\n")
        .with_no_cache()
        .with_build_log_consumer([&](std::string_view line) { log.append(line); })
        .build();

    EXPECT_NE(log.find("Step"), std::string::npos) << "log was: " << log;
    EXPECT_NE(log.find("consumer-sees-this"), std::string::npos) << "log was: " << log;
}

TEST_F(BuildImage, BuildFailureCarriesStepOutput) {
    // The failing RUN echoes a marker before exiting: that output — the part a
    // human needs to debug the step — must be inside the thrown error, even
    // with no consumer attached.
    try {
        GenericBuildableImage("tc-build-fail-output", "latest")
            .with_dockerfile_string("FROM alpine:3.20\n"
                                    "RUN echo boom-diagnostic-marker && exit 7\n")
            .with_no_cache()
            .build();
        FAIL() << "build() must throw on a failing RUN";
    } catch (const DockerError& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("boom-diagnostic-marker"), std::string::npos) << what;
        // The daemon's own failure line, e.g. "... returned a non-zero code: 7".
        EXPECT_NE(what.find("non-zero code: 7"), std::string::npos) << what;
    }
}

TEST_F(BuildImage, DockerignoreFiltersContext) {
    // A directory context whose root .dockerignore excludes *.log and a whole
    // subdirectory: the excluded files must never reach the daemon, while the
    // .dockerignore itself still ships (docker build parity). COPY the whole
    // context in and list it to see exactly what arrived.
    struct ContextDir {
        std::filesystem::path dir;
        ContextDir() {
            dir = std::filesystem::temp_directory_path() /
                  ("tc_buildignore_" + tcit::random_suffix());
            std::filesystem::create_directories(dir / "logs");
            std::ofstream(dir / "keep.txt", std::ios::binary) << "keep-me";
            std::ofstream(dir / "skip.log", std::ios::binary) << "skip-me";
            std::ofstream(dir / "logs" / "nested.log", std::ios::binary) << "nested";
            std::ofstream(dir / ".dockerignore", std::ios::binary) << "*.log\nlogs\n";
        }
        ~ContextDir() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    } context;

    GenericImage image = GenericBuildableImage("tc-build-ignore", "latest")
                             .with_dockerfile_string("FROM alpine:3.20\n"
                                                     "COPY . /ctx\n"
                                                     "CMD [\"ls\", \"-A\", \"/ctx\"]\n")
                             .with_file(context.dir, "")
                             .build();

    Container c = image.with_wait(wait_for::exit()).start();

    const ContainerLogs out = c.logs();
    EXPECT_NE(out.stdout_data.find("keep.txt"), std::string::npos)
        << "stdout was: " << out.stdout_data;
    EXPECT_NE(out.stdout_data.find(".dockerignore"), std::string::npos)
        << "stdout was: " << out.stdout_data;
    EXPECT_EQ(out.stdout_data.find("skip.log"), std::string::npos)
        << "stdout was: " << out.stdout_data;
    EXPECT_EQ(out.stdout_data.find("logs"), std::string::npos) << "stdout was: " << out.stdout_data;
}

TEST_F(BuildImage, BuiltImageCarriesSessionLabels) {
    // Built images are session-scoped: build() ships the managed-by/session
    // labels via ?labels= (so Ryuk reaps the image after the run) and they
    // MERGE with the Dockerfile's own LABELs rather than replacing them. On a
    // duplicate key the query label wins (docker build --label parity) — the
    // Dockerfile below tries to override managed-by and must lose.
    GenericBuildableImage("tc-build-labels", "latest")
        .with_dockerfile_string("FROM alpine:3.20\n"
                                "LABEL tc-user-label=kept\n"
                                "LABEL org.testcontainers.managed-by=dockerfile-tried\n")
        .build();

    const ImageInspect info = GenericImage::inspect("tc-build-labels");
    ASSERT_EQ(info.labels.count("org.testcontainers.managed-by"), 1u);
    EXPECT_EQ(info.labels.at("org.testcontainers.managed-by"), "testcontainers");
    ASSERT_EQ(info.labels.count("tc-user-label"), 1u);
    EXPECT_EQ(info.labels.at("tc-user-label"), "kept");
    if (!detail::ryuk_disabled()) {
        ASSERT_EQ(info.labels.count("org.testcontainers.session-id"), 1u);
        EXPECT_EQ(info.labels.at("org.testcontainers.session-id"), detail::session_id());

        // build() must have booted the reaper even with no container started
        // (a labelled image without a watching Ryuk would never be reaped).
        DockerClient client = DockerClient::from_environment();
        const std::vector<ContainerSummary> running = client.list_containers(
            {{"label", "org.testcontainers.managed-by=testcontainers"}}, /*all*/ false);
        const bool found_ryuk =
            std::any_of(running.begin(), running.end(), [](const ContainerSummary& c) {
                return c.image.find("testcontainers/ryuk") != std::string::npos;
            });
        EXPECT_TRUE(found_ryuk) << "no running testcontainers/ryuk container found";
    }
}

TEST_F(BuildImage, ExistsReflectsLocalImages) {
    // Self-contained: build the probe image here rather than depending on
    // another test's tag.
    GenericBuildableImage("tc-exists-probe", "latest")
        .with_dockerfile_string("FROM alpine:3.20\n")
        .build();

    // Name and tag are separate (constructor-style); the tag defaults to "latest".
    EXPECT_TRUE(GenericImage::exists("tc-exists-probe"));
    EXPECT_TRUE(GenericImage::exists("tc-exists-probe", "latest"));
    EXPECT_FALSE(GenericImage::exists("tc-definitely-never-built", "v9"));
}

TEST_F(BuildImage, InspectReflectsImageConfig) {
    // Self-contained: bake distinctive Config fields to assert on.
    GenericBuildableImage("tc-inspect-probe", "latest")
        .with_dockerfile_string("FROM alpine:3.20\n"
                                "LABEL tc-inspect-test=yes\n"
                                "EXPOSE 8125/tcp\n"
                                "WORKDIR /srv\n"
                                "CMD [\"sleep\", \"1\"]\n")
        .build();

    // The static lookup: name and tag separate, tag defaulting to "latest".
    const ImageInspect info = GenericImage::inspect("tc-inspect-probe");
    EXPECT_TRUE(info.id.starts_with("sha256:")) << info.id;
    EXPECT_EQ(info.os, "linux");
    EXPECT_GT(info.size, 0);
    ASSERT_FALSE(info.repo_tags.empty());
    EXPECT_NE(std::find(info.repo_tags.begin(), info.repo_tags.end(),
                        std::string("tc-inspect-probe:latest")),
              info.repo_tags.end());
    ASSERT_EQ(info.labels.count("tc-inspect-test"), 1u);
    EXPECT_EQ(info.labels.at("tc-inspect-test"), "yes");
    ASSERT_EQ(info.exposed_ports.size(), 1u);
    EXPECT_EQ(info.exposed_ports[0], "8125/tcp");
    EXPECT_EQ(info.working_dir, "/srv");
    ASSERT_EQ(info.cmd.size(), 2u);
    EXPECT_EQ(info.cmd[0], "sleep");

    // The instance form inspects this config's own image():tag().
    EXPECT_EQ(GenericImage("tc-inspect-probe").inspect().id, info.id);

    // The raw escape hatch returns the same inspect body, unparsed.
    EXPECT_NE(DockerClient::from_environment()
                  .inspect_image_raw("tc-inspect-probe:latest")
                  .find("\"Id\""),
              std::string::npos);

    // A reference that never existed surfaces as NotFoundError (404).
    EXPECT_THROW(GenericImage::inspect("tc-definitely-never-built", "v9"), NotFoundError);
}

// The Windows mirror. The FROM line must name the tag matching the daemon
// host's build (process isolation), so the Dockerfile is assembled at runtime.
class WindowsBuildImage : public tcit::WindowsEngineTest {
protected:
    std::string from_line() const {
        return "FROM " + std::string(tcit::kWindowsImage) + ":" + tag_ + "\n";
    }
};

TEST_F(WindowsBuildImage, BuildsAndRunsInlineDockerfile) {
    // RUN uses the Windows default shell (`cmd /S /C`) with cwd C:\, so the
    // file lands at C:\built.txt without any backslashes in the Dockerfile
    // (backslash is its escape character). nanoserver's default user
    // (ContainerUser) cannot write to C:\, hence USER ContainerAdministrator.
    GenericImage image =
        GenericBuildableImage("tc-build-test-win", "latest")
            .with_dockerfile_string(from_line() + "USER ContainerAdministrator\n"
                                                  "RUN echo built-content> built.txt\n"
                                                  "CMD [\"cmd\", \"/c\", \"type built.txt\"]")
            .build();

    Container c = image.with_wait(wait_for::exit()).start();

    const ContainerLogs out = c.logs();
    EXPECT_NE(out.stdout_data.find("built-content"), std::string::npos)
        << "stdout was: " << out.stdout_data;
}

TEST_F(WindowsBuildImage, BuildFailureThrows) {
    // cmd's `exit 3` fails the RUN step; the daemon still answers HTTP 200 with
    // the error embedded in the build stream, so build() must throw.
    EXPECT_THROW(GenericBuildableImage("tc-build-fail-win", "latest")
                     .with_dockerfile_string(from_line() + "RUN exit 3")
                     .build(),
                 DockerError);
}

TEST_F(WindowsBuildImage, ExistsAndBuildLogConsumer) {
    // One mirror covers the daemon-facing pieces on Windows: the consumer sees
    // the streamed step output, exists() reflects the built tag, and inspect()
    // reads the image back.
    std::string log;
    GenericBuildableImage("tc-exists-probe-win", "latest")
        .with_dockerfile_string(from_line() + "USER ContainerAdministrator\n"
                                              "RUN echo win-consumer-sees-this\n")
        .with_no_cache()
        .with_build_log_consumer([&](std::string_view line) { log.append(line); })
        .build();

    EXPECT_NE(log.find("win-consumer-sees-this"), std::string::npos) << "log was: " << log;
    EXPECT_TRUE(GenericImage::exists("tc-exists-probe-win"));
    EXPECT_FALSE(GenericImage::exists("tc-definitely-never-built-win", "v9"));

    const ImageInspect info = GenericImage::inspect("tc-exists-probe-win");
    EXPECT_TRUE(info.id.starts_with("sha256:")) << info.id;
    EXPECT_EQ(info.os, "windows");
    ASSERT_FALSE(info.repo_tags.empty());
    EXPECT_NE(std::find(info.repo_tags.begin(), info.repo_tags.end(),
                        std::string("tc-exists-probe-win:latest")),
              info.repo_tags.end());
}
