#pragma once

#include <functional>
#include <string>

namespace testcontainers {

class DockerClient; // fwd-decl is enough for the typedef

/// A lifecycle hook: receives the Docker client and the container id at a
/// defined point in the container's lifecycle. A hook that throws aborts start()
/// for the *-starting/-started/-created points (the partial container is then
/// cleaned up); a *-stopping hook that throws is swallowed (teardown is
/// best-effort and must never propagate, especially from the destructor).
using LifecycleHook = std::function<void(DockerClient&, const std::string&)>;

} // namespace testcontainers
