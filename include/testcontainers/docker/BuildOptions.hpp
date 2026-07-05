#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace testcontainers::docker {

/// Receives decoded build output as the daemon emits it: one call per "stream"
/// payload of the build progress (typically a single newline-terminated line,
/// e.g. "Step 2/4 : RUN make\n" or a RUN step's own output). Called from the
/// thread running the build. An empty function disables streaming delivery.
/// An exception thrown by the consumer aborts the build read (the connection
/// is closed) and propagates to the caller unchanged.
using BuildLogConsumer = std::function<void(std::string_view)>;

/// Options for `POST /build` — the knobs the Docker build endpoint accepts that
/// we surface. A plain, copyable value type (std only): the tar build context is
/// supplied separately as the request body.
struct BuildOptions {
    std::string tag;                       ///< t= (e.g. "myimg:latest")
    std::string dockerfile = "Dockerfile"; ///< dockerfile= (path within the context)
    std::vector<std::pair<std::string, std::string>> build_args; ///< buildargs= (JSON-encoded map)
    std::string target;    ///< target= (multi-stage target stage)
    bool no_cache = false; ///< nocache=
    bool pull = false;     ///< pull= (always re-pull the base image)
};

} // namespace testcontainers::docker
