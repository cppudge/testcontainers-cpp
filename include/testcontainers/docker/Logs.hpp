#pragma once

#include <string>

namespace testcontainers {

/// Options for `GET /containers/{id}/logs`.
///
/// Note: the include-flags are named `include_stdout` / `include_stderr` rather
/// than `stdout` / `stderr` because the latter are reserved macros from <cstdio>
/// on some platforms (notably MSVC, where `stdout` expands to a function call).
struct LogOptions {
    bool include_stdout = true; ///< include the stdout stream
    bool include_stderr = true; ///< include the stderr stream
    bool follow = false;        ///< stream until the container exits (vs. snapshot)
    std::string tail = "all";   ///< number of trailing lines, or "all"
    bool timestamps = false;    ///< prefix each line with an RFC3339 timestamp
};

/// Combined stdout / stderr text retrieved from a container.
struct ContainerLogs {
    std::string stdout_data;
    std::string stderr_data;
};

} // namespace testcontainers
