#pragma once

#include <cstdint>
#include <string>

namespace testcontainers {

/// A process resource limit (HostConfig.Ulimits entry), e.g. {"nofile", 1024, 2048}.
struct Ulimit {
    std::string name;     ///< e.g. "nofile", "nproc"
    std::int64_t soft = 0;
    std::int64_t hard = 0;
};

} // namespace testcontainers
