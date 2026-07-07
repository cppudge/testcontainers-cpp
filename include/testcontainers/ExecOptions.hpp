#pragma once

#include <optional>
#include <string>
#include <vector>

namespace testcontainers {

/// Options for running a command inside a container via `exec`.
///
/// All fields are plain std types so the public header stays free of any
/// third-party (Boost / nlohmann / asio) dependency.
struct ExecOptions {
    std::vector<std::string> env;           ///< each "KEY=VALUE" (Env)
    std::optional<std::string> working_dir; ///< WorkingDir
    std::optional<std::string> user;        ///< User ("name" or "uid[:gid]")
    bool privileged = false;                ///< Privileged
    bool tty = false;                       ///< allocate a TTY (raw, unframed output)
    /// Fire-and-forget (`docker exec -d`): start the command and return
    /// immediately instead of waiting for it to finish. Nothing is attached or
    /// captured — the returned ExecResult keeps its defaults (empty output,
    /// exit_code 0; the command is still running, so its real status is
    /// unknown; a command that fails inside the container surfaces no error).
    /// Cannot be combined with `stdin_data` or with the streaming (consumer)
    /// exec overload: those combinations throw DockerError before any daemon
    /// interaction.
    bool detach = false;
    /// When set, attach stdin, feed these bytes, then half-close so the reader
    /// sees EOF. Requires a half-closable transport (TCP / unix socket): on the
    /// Windows named-pipe and TLS transports exec throws instead (no EOF signal
    /// is possible there, so a reader like `cat` would hang forever).
    std::optional<std::string> stdin_data;
};

} // namespace testcontainers
