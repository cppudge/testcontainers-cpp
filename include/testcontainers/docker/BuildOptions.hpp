#pragma once

#include <string>
#include <utility>
#include <vector>

namespace testcontainers::docker {

/// Options for `POST /build` — the knobs the Docker build endpoint accepts that
/// we surface. A plain, copyable value type (std only): the tar build context is
/// supplied separately as the request body.
struct BuildOptions {
    std::string tag;                                              ///< t= (e.g. "myimg:latest")
    std::string dockerfile = "Dockerfile";                       ///< dockerfile= (path within the context)
    std::vector<std::pair<std::string, std::string>> build_args; ///< buildargs= (JSON-encoded map)
    std::string target;                                          ///< target= (multi-stage target stage)
    bool no_cache = false;                                       ///< nocache=
    bool pull = false;                                           ///< pull= (always re-pull the base image)
};

} // namespace testcontainers::docker
