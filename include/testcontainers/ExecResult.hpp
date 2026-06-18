#pragma once

#include <cstdint>
#include <string>

namespace testcontainers {

/// The result of running a command inside a container via `exec`.
///
/// `stdout_data` / `stderr_data` are the demultiplexed output streams; the
/// fields are named with a `_data` suffix because `stdout` / `stderr` are
/// reserved macros from <cstdio> on some platforms (notably MSVC).
struct ExecResult {
    std::string stdout_data;
    std::string stderr_data;
    std::int64_t exit_code = 0;
};

} // namespace testcontainers
