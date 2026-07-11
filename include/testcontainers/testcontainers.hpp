#pragma once

/// Umbrella header for the public testcontainers-cpp API.
/// As the API grows, the individual public headers are included from here.

#include "testcontainers/ConnectionString.hpp"
#include "testcontainers/Container.hpp"
#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/DockerComposeContainer.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/GenericBuildableImage.hpp"
#include "testcontainers/GenericImage.hpp"
#include "testcontainers/Ulimit.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/docker/DockerClient.hpp"
#include "testcontainers/docker/DockerHost.hpp"
#include "testcontainers/version.hpp"
