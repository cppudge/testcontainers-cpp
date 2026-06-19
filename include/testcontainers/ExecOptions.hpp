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
    std::vector<std::string> env;            ///< each "KEY=VALUE" (Env)
    std::optional<std::string> working_dir;  ///< WorkingDir
    std::optional<std::string> user;         ///< User ("name" or "uid[:gid]")
    bool privileged = false;                 ///< Privileged
    bool tty = false;                        ///< allocate a TTY (raw, unframed output)
    std::optional<std::string> stdin_data;   ///< when set, attach stdin and feed these bytes
};

} // namespace testcontainers
