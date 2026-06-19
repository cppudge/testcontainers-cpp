#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

// A small cross-platform subprocess helper used ONLY by the local compose
// client (the documented compose-only exception to the library's "no docker
// CLI" rule). No Boost here — just the C runtime's popen/pclose.

namespace testcontainers::compose {

/// The outcome of running a child process: its exit code and combined output.
struct ProcessResult {
    int exit_code = 0;  ///< the process exit status (WEXITSTATUS on POSIX)
    std::string output; ///< merged stdout + stderr (we append `2>&1`)
};

/// Run `argv` as a child process, capturing its merged stdout+stderr.
///
/// Implementation: we shell out via `_popen`/`_pclose` (`popen`/`pclose` on
/// POSIX), so the argv is joined into a single command line. Each element is
/// quoted defensively (wrapped in double quotes with embedded double quotes
/// escaped) so paths/values with spaces survive; `2>&1` is appended to fold
/// stderr into the captured stream. The exit code is the raw value on Windows
/// and `WEXITSTATUS(status)` on POSIX.
///
/// `working_dir`, when set, is applied by prefixing a `cd` into it — but the
/// compose clients avoid this by passing absolute `-f` paths (and compose's own
/// `--project-directory`), so it is normally nullopt.
///
/// `env` entries are exported into the process environment via `_putenv_s` /
/// `setenv` immediately before the run and RESTORED to their prior values (or
/// unset if previously absent) immediately after, so the parent environment is
/// left untouched.
ProcessResult run_process(const std::vector<std::string>& argv,
                          const std::optional<std::string>& working_dir = std::nullopt,
                          const std::vector<std::pair<std::string, std::string>>& env = {});

} // namespace testcontainers::compose
