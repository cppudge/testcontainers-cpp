#include <gtest/gtest.h>

#include <string>

#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/DockerComposeContainer.hpp"

// Tests in this file (daemon-free; nothing here calls start()):
//   DockerComposeContainer.DefaultProjectName - a fresh handle has a non-empty project name starting with "tc".
//   DockerComposeContainer.FromYamlStoresYaml - from_yaml stores the compose YAML verbatim and defaults the ambassador image.
//   DockerComposeContainer.WithProjectName - with_project_name overrides the project name.
//   DockerComposeContainer.WithComposeImage - with_compose_image overrides the ambassador image.
//   DockerComposeContainer.UnknownServiceThrows - querying a service before start() (none discovered) throws.

using namespace testcontainers;

TEST(DockerComposeContainer, DefaultProjectName) {
    const DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");
    EXPECT_FALSE(compose.project_name().empty());
    EXPECT_EQ(compose.project_name().substr(0, 2), "tc");
}

TEST(DockerComposeContainer, FromYamlStoresYaml) {
    const std::string yaml = "services:\n  redis:\n    image: redis:7.2\n";
    const DockerComposeContainer compose = DockerComposeContainer::from_yaml(yaml);
    EXPECT_EQ(compose.compose_yaml(), yaml);
    // Default ambassador image (its entrypoint is docker-compose).
    EXPECT_EQ(compose.compose_image(), "docker/compose:1.29.2");
}

TEST(DockerComposeContainer, WithProjectName) {
    DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");
    compose.with_project_name("my-project");
    EXPECT_EQ(compose.project_name(), "my-project");
}

TEST(DockerComposeContainer, WithComposeImage) {
    DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");
    compose.with_compose_image("docker/compose:2.0.0");
    EXPECT_EQ(compose.compose_image(), "docker/compose:2.0.0");
}

TEST(DockerComposeContainer, UnknownServiceThrows) {
    DockerComposeContainer compose = DockerComposeContainer::from_yaml("services: {}\n");
    compose.with_exposed_service("redis", tcp(6379));
    // No start() ran, so nothing is discovered; looking the service up must throw
    // (purely a map lookup — no daemon involved).
    EXPECT_ANY_THROW(compose.get_service_container_id("redis"));
}
