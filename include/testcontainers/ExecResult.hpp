#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "testcontainers/docker/Logs.hpp"

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

/// The result of a deadline-bounded streaming `exec` (the overload taking a
/// deadline): why output delivery ended, plus the exit code when the command
/// had actually finished by then. Stopping delivery does NOT kill the command
/// — after DeadlineExpired (or a consumer stop mid-run) it keeps running
/// inside the container — so `exit_code` is empty in those cases.
struct ExecStreamResult {
    FollowEnd end = FollowEnd::StreamEnded; ///< why output delivery ended
    /// Exit code, read back from the exec inspect after the stream ended;
    /// present only when the inspect says the command had finished (it
    /// virtually always has after StreamEnded; after an early stop it is a
    /// race).
    std::optional<std::int64_t> exit_code;
};

} // namespace testcontainers
