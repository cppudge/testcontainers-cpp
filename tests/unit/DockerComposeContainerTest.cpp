#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/DockerComposeContainer.hpp"

// Tests in this file (daemon-free; nothing here calls start()):
//   DockerComposeContainer.DefaultProjectName - a fresh handle has a non-empty project name starting with "tc".
//   DockerComposeContainer.DefaultClientIsLocal - the default client kind is Local (matches rust's default).
//   DockerComposeContainer.DefaultComposeImage - the default containerised ambassador image is docker:26.1-cli.
//   DockerComposeContainer.FromYamlYieldsOneFileAndLocal - from_yaml writes one temp compose file and defaults to Local.
//   DockerComposeContainer.FactoriesSetKindAndFiles - the with_*_client factories set the right kind and carry the files.
//   DockerComposeContainer.WithClientOverridesKind - with_client overrides the kind on an existing instance.
//   DockerComposeContainer.WithProjectName - with_project_name overrides the project name.
//   DockerComposeContainer.WithComposeImage - with_compose_image overrides the ambassador image.
//   DockerComposeContainer.EnvGetters - with_env / with_env_vars reflect in env().
//   DockerComposeContainer.ProfilesAccumulateInOrder - profiles() is empty by default; with_profile appends in call order (rvalue chaining included).
//   DockerComposeContainer.FlagGetters - with_build/pull/wait/wait_timeout/remove_volumes/remove_images reflect in the getters.
//   DockerComposeContainer.UnknownServiceThrows - querying a service before start() (none discovered) throws.
//   DockerComposeContainer.MoveConstructTransfersTempFileOwnership - after a move the source's destructor leaves the from_yaml temp file alone; only the target's destructor deletes it.
//   DockerComposeContainer.MoveAssignTransfersState - move-assignment carries config over and the moved-from handle tears nothing down.
//   DockerComposeContainer.MoveAssignReleasesTargetsOldTempFile - move-assigning over a handle releases (deletes) the temp file the target owned before.
//   DockerComposeContainer.StopIsIdempotent - stop() before start() and repeated stop() are harmless; the from_yaml temp file is gone after the first stop().

using namespace testcontainers;

TEST(DockerComposeContainer, DefaultProjectName) {
    const DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");
    EXPECT_FALSE(compose.project_name().empty());
    EXPECT_EQ(compose.project_name().substr(0, 2), "tc");
}

TEST(DockerComposeContainer, DefaultClientIsLocal) {
    const DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");
    EXPECT_EQ(compose.client_kind(), ComposeClientKind::Local);
}

TEST(DockerComposeContainer, DefaultComposeImage) {
    const DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");
    // Containerised ambassador image is a long-lived docker:cli (compose v2).
    EXPECT_EQ(compose.compose_image(), "docker:26.1-cli");
}

TEST(DockerComposeContainer, FromYamlYieldsOneFileAndLocal) {
    const DockerComposeContainer compose =
        DockerComposeContainer::from_yaml("services:\n  redis:\n    image: redis:7.2\n");
    ASSERT_EQ(compose.compose_files().size(), 1u);
    EXPECT_FALSE(compose.compose_files().front().empty());
    EXPECT_EQ(compose.client_kind(), ComposeClientKind::Local);
}

TEST(DockerComposeContainer, FactoriesSetKindAndFiles) {
    const std::vector<std::string> files = {"a.yml", "b.yml"};

    const DockerComposeContainer local = DockerComposeContainer::with_local_client(files);
    EXPECT_EQ(local.client_kind(), ComposeClientKind::Local);
    EXPECT_EQ(local.compose_files().size(), 2u);

    const DockerComposeContainer cont = DockerComposeContainer::with_containerised_client(files);
    EXPECT_EQ(cont.client_kind(), ComposeClientKind::Containerised);
    EXPECT_EQ(cont.compose_files().size(), 2u);

    const DockerComposeContainer auto_c = DockerComposeContainer::with_auto_client(files);
    EXPECT_EQ(auto_c.client_kind(), ComposeClientKind::Auto);
    EXPECT_EQ(auto_c.compose_files().size(), 2u);
}

TEST(DockerComposeContainer, WithClientOverridesKind) {
    DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");
    EXPECT_EQ(compose.client_kind(), ComposeClientKind::Local);
    compose.with_client(ComposeClientKind::Containerised);
    EXPECT_EQ(compose.client_kind(), ComposeClientKind::Containerised);
}

TEST(DockerComposeContainer, WithProjectName) {
    DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");
    compose.with_project_name("my-project");
    EXPECT_EQ(compose.project_name(), "my-project");
}

TEST(DockerComposeContainer, WithComposeImage) {
    DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");
    compose.with_compose_image("docker:27-cli");
    EXPECT_EQ(compose.compose_image(), "docker:27-cli");
}

TEST(DockerComposeContainer, EnvGetters) {
    DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");
    compose.with_env("FOO", "bar");
    compose.with_env_vars(std::map<std::string, std::string>{{"BAZ", "qux"}, {"FOO", "override"}});

    const auto& env = compose.env();
    ASSERT_EQ(env.count("FOO"), 1u);
    ASSERT_EQ(env.count("BAZ"), 1u);
    EXPECT_EQ(env.at("FOO"), "override"); // with_env_vars merges over with_env
    EXPECT_EQ(env.at("BAZ"), "qux");
}

TEST(DockerComposeContainer, ProfilesAccumulateInOrder) {
    {
        const DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");
        EXPECT_TRUE(compose.profiles().empty());
    }

    // Repeatable, order-preserving; the && overload chains on a temporary.
    const DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n")
                                               .with_profile("frontend")
                                               .with_profile("debug");
    EXPECT_EQ(compose.profiles(), (std::vector<std::string>{"frontend", "debug"}));
}

TEST(DockerComposeContainer, FlagGetters) {
    DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");

    // Defaults: build off, pull off, wait on (60s), remove_volumes on, remove_images off.
    EXPECT_FALSE(compose.build());
    EXPECT_FALSE(compose.pull());
    EXPECT_TRUE(compose.wait());
    EXPECT_EQ(compose.wait_timeout(), std::chrono::seconds(60));
    EXPECT_TRUE(compose.remove_volumes());
    EXPECT_FALSE(compose.remove_images());

    compose.with_build().with_pull().with_wait(false).with_wait_timeout(std::chrono::seconds(120));
    compose.with_remove_volumes(false).with_remove_images(true);

    EXPECT_TRUE(compose.build());
    EXPECT_TRUE(compose.pull());
    EXPECT_FALSE(compose.wait());
    EXPECT_EQ(compose.wait_timeout(), std::chrono::seconds(120));
    EXPECT_FALSE(compose.remove_volumes());
    EXPECT_TRUE(compose.remove_images());
}

TEST(DockerComposeContainer, UnknownServiceThrows) {
    DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");
    compose.with_exposed_service("redis", tcp(6379));
    // No start() ran, so nothing is discovered; looking the service up must throw
    // (purely a map lookup — no daemon involved).
    EXPECT_ANY_THROW(compose.get_service_container_id("redis"));
}

TEST(DockerComposeContainer, MoveConstructTransfersTempFileOwnership) {
    // A moved-from handle must own nothing, so its destructor tears nothing
    // down. Observable via the from_yaml temp file: destroying the moved-from
    // source must NOT delete it; destroying the target must.
    //
    // std::optional makes the move ctor + moved-from destruction UNAMBIGUOUS:
    // a `return source;` from a helper would be NRVO-elided (GCC/Clang do it
    // even at -O0), silently skipping the very move this test exists to check.
    std::string temp_path;
    {
        std::optional<DockerComposeContainer> source(
            DockerComposeContainer::from_yaml("services: {}\n"));
        temp_path = source->compose_files().front();
        ASSERT_TRUE(std::filesystem::exists(temp_path));

        const DockerComposeContainer target(std::move(*source)); // the move ctor, guaranteed
        source.reset(); // the moved-from handle's destructor runs NOW

        // The moved-from source was destroyed; the temp file must have survived.
        EXPECT_TRUE(std::filesystem::exists(temp_path));
        EXPECT_EQ(target.compose_files().front(), temp_path);
        EXPECT_EQ(target.client_kind(), ComposeClientKind::Local);
    }
    // The target's destructor owns (and deletes) the temp file.
    EXPECT_FALSE(std::filesystem::exists(temp_path));
}

TEST(DockerComposeContainer, MoveAssignTransfersState) {
    std::string temp_path;
    {
        DockerComposeContainer target = DockerComposeContainer::with_local_client({"a.yml"});
        {
            DockerComposeContainer source = DockerComposeContainer::from_yaml("services: {}\n");
            source.with_project_name("moved-project").with_wait_timeout(std::chrono::seconds(5));
            temp_path = source.compose_files().front();

            target = std::move(source);
            // The moved-from source is destroyed here and must not delete the file.
        }
        EXPECT_TRUE(std::filesystem::exists(temp_path));
        EXPECT_EQ(target.project_name(), "moved-project");
        EXPECT_EQ(target.wait_timeout(), std::chrono::seconds(5));
        EXPECT_EQ(target.compose_files().front(), temp_path);
    }
    EXPECT_FALSE(std::filesystem::exists(temp_path));
}

TEST(DockerComposeContainer, MoveAssignReleasesTargetsOldTempFile) {
    // The RAII promise on assignment: the target's OWN resources are released
    // before adopting the source's — its old temp file must be deleted at the
    // assignment, not leaked.
    DockerComposeContainer target = DockerComposeContainer::from_yaml("services: {}\n");
    const std::string old_temp = target.compose_files().front();
    ASSERT_TRUE(std::filesystem::exists(old_temp));

    DockerComposeContainer source = DockerComposeContainer::from_yaml("services: {}\n");
    const std::string new_temp = source.compose_files().front();

    target = std::move(source);
    EXPECT_FALSE(std::filesystem::exists(old_temp)) << "the target's old temp file leaked";
    EXPECT_TRUE(std::filesystem::exists(new_temp));
    EXPECT_EQ(target.compose_files().front(), new_temp);
}

TEST(DockerComposeContainer, StopIsIdempotent) {
    DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");
    const std::string temp_path = compose.compose_files().front();
    ASSERT_TRUE(std::filesystem::exists(temp_path));

    // Never started: stop() must be harmless, and it releases the temp file.
    compose.stop();
    EXPECT_FALSE(std::filesystem::exists(temp_path));
    compose.stop(); // and again
    // The handle is still queryable (config is untouched by stop()).
    EXPECT_EQ(compose.client_kind(), ComposeClientKind::Local);
}
