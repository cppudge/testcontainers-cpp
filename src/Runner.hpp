#pragma once

#include "testcontainers/Container.hpp"
#include "testcontainers/ContainerRequest.hpp"

namespace testcontainers::detail {

/// The container-start orchestration core: reuse lookup → startup-attempt
/// retry → create → copy-to → lifecycle hooks → start → wait-until-ready →
/// handle construction. Split out of `GenericImage::start()` so the whole
/// sequence can be unit-tested by pointing `client` at a canned loopback
/// responder (`tests/unit/RunnerTest.cpp`).
///
/// Does NOT bootstrap the Reaper — the public `run()` wrappers do that (a unit
/// test driving this core must not start Ryuk against a real daemon).
class Runner {
public:
    static Container run(DockerClient& client, const ContainerRequest& request);
};

} // namespace testcontainers::detail
