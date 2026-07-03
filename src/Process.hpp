#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

// A small cross-platform subprocess helper. Originally written for the local
// compose client (the documented compose-only exception to the library's "no
// docker CLI" rule), it is also used to drive Docker credential helpers. No
// Boost here — just the C runtime's popen/pclose.

namespace testcontainers::detail {

/// The outcome of running a child process: its exit code and combined output.
struct ProcessResult {
    int exit_code = 0;  ///< the process exit status (WEXITSTATUS on POSIX)
    std::string output; ///< merged stdout + stderr (we append `2>&1`)
};

/// Quote a single argv element for inclusion in a shell command line.
///
/// Wraps the element in double quotes and escapes any embedded double quote
/// (`"` -> `\"`). This keeps paths/values containing spaces intact. We do NOT
/// attempt full POSIX/cmd metacharacter escaping — the argv here is
/// library-controlled (compose subcommands + paths), not arbitrary user shell
/// input; see TODO.md for the cmd.exe embedded-quote caveat. Exposed (pure)
/// for unit testing.
std::string quote_arg(const std::string& arg);

/// Build the full command line run_process hands to popen: each argv element
/// quoted and space-joined, an optional `cd "<dir>" &&` prefix, an optional
/// `< "<stdin-file>"` redirection, and a trailing `2>&1`. On Windows the whole
/// line is wrapped in one more pair of quotes so cmd.exe's first/last-quote
/// stripping is a no-op on our real quoting. Exposed (pure) for unit testing.
std::string build_command_line(const std::vector<std::string>& argv,
                               const std::optional<std::string>& working_dir,
                               const std::optional<std::string>& stdin_file);

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
///
/// `stdin_data`, when set, is fed to the child on its stdin. Since popen is
/// unidirectional, we write the data to a temp file and append an (unquoted)
/// `< "<tempfile>"` redirection to the command line; the temp file is deleted
/// (best-effort) after the child exits. When nullopt the child inherits the
/// parent's stdin (the original behaviour).
ProcessResult run_process(const std::vector<std::string>& argv,
                          const std::optional<std::string>& working_dir = std::nullopt,
                          const std::vector<std::pair<std::string, std::string>>& env = {},
                          const std::optional<std::string>& stdin_data = std::nullopt);

} // namespace testcontainers::detail
