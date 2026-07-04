#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

// A small cross-platform subprocess helper. Originally written for the local
// compose client (the documented compose-only exception to the library's "no
// docker CLI" rule), it is also used to drive Docker credential helpers.
// Children are spawned directly (CreateProcessW / posix_spawnp) — no shell in
// between — with an EXPLICIT environment block, so the parent environment is
// never mutated and concurrent run_process calls cannot cross-contaminate.

namespace testcontainers::detail {

/// The outcome of running a child process: its exit code and combined output.
struct ProcessResult {
    int exit_code = 0;  ///< the process exit status (WEXITSTATUS on POSIX)
    std::string output; ///< merged stdout + stderr (both handed the same pipe)
};

/// Quote a single argv element for a Windows child's command line.
///
/// Follows the CommandLineToArgvW / MSVCRT parsing rules the child applies:
/// the element is wrapped in double quotes, an embedded `"` becomes `\"` with
/// any backslash run before it doubled, and a trailing backslash run is
/// doubled so the closing quote stays a delimiter. Only used to build Windows
/// command lines (POSIX passes argv directly); compiled everywhere as a pure
/// function for unit testing.
std::string quote_arg(const std::string& arg);

/// Build the command line handed to CreateProcessW: each argv element quoted
/// per quote_arg and space-joined. No shell is involved, so there is no
/// redirection or cmd.exe-quoting layer here. Exposed (pure) for unit testing.
std::string build_command_line(const std::vector<std::string>& argv);

/// Run `argv` as a child process, capturing its merged stdout+stderr.
///
/// CONTRACT: `argv[0]` must be a real executable — the child is spawned
/// directly via CreateProcessW (which appends `.exe` and searches PATH) or
/// posix_spawnp; there is no shell, so shell builtins and `.bat`/`.cmd`
/// scripts are not runnable. Every library caller passes one (docker,
/// compose, docker-credential-<helper>).
///
/// `working_dir`, when set, becomes the child's working directory natively
/// (CreateProcessW's lpCurrentDirectory / posix_spawn addchdir) — the compose
/// clients avoid it by passing absolute `-f` paths, so it is normally nullopt.
///
/// `env` entries OVERRIDE the inherited variables in an explicit environment
/// block built for the child; the parent's own environment is never touched,
/// making concurrent run_process calls with different env safe.
///
/// `stdin_data`, when set, is staged in a temp file opened as the child's
/// stdin (deleted best-effort after the run) — a file cannot deadlock against
/// the output pipe the way a stdin pipe could. When nullopt the child reads
/// EOF (NUL / /dev/null); no library child reads stdin outside `stdin_data`.
ProcessResult run_process(const std::vector<std::string>& argv,
                          const std::optional<std::string>& working_dir = std::nullopt,
                          const std::vector<std::pair<std::string, std::string>>& env = {},
                          const std::optional<std::string>& stdin_data = std::nullopt);

} // namespace testcontainers::detail
