#pragma once

#include <string>
#include <utility>
#include <vector>

// Pure, daemon-free compose command model + argv builders.
//
// These structs describe a `docker compose up` / `down` invocation in the
// abstract; the builders turn them into the argv that follows `docker compose`
// (i.e. starting at `--project-name`). They contain NO Boost / DockerClient /
// filesystem dependency so the unit tests can assert on the produced argv
// without a daemon. Both the local and the containerised clients build their
// command lines through these helpers (the only difference is the `files` they
// pass — host paths vs in-container `/docker-compose-<i>.yml` paths).

namespace testcontainers::compose {

/// A `docker compose ... up -d` invocation, described abstractly.
struct ComposeUpCommand {
    std::string project_name;          ///< --project-name <p>
    std::vector<std::string> files;    ///< each emitted as `-f <file>`
    std::vector<std::string> profiles; ///< each emitted as `--profile <name>`
    /// Environment passed to the child process (local) / exec (containerised).
    /// Not part of the argv — recorded here so the clients can read it.
    std::vector<std::pair<std::string, std::string>> env;
    long long wait_timeout_secs = 60; ///< --wait-timeout <secs> (when wait)
    bool build = false;               ///< --build
    bool pull = false;                ///< --pull always
    bool wait = true;                 ///< --wait --wait-timeout <secs>
};

/// A `docker compose ... down` invocation, described abstractly.
struct ComposeDownCommand {
    std::string project_name; ///< --project-name <p>
    /// The same profiles as `up`: a file-driven `down` (the local client passes
    /// `-f`) enumerates its services from the file model filtered by ACTIVE
    /// profiles, so without them it leaves profile-gated containers behind
    /// (with an orphan warning). A label-reconstructed `down` (no `-f`, the
    /// containerised client) removes them regardless — the flags are a
    /// harmless no-op there.
    std::vector<std::string> profiles; ///< each emitted as `--profile <name>`
    /// Environment passed to the child process (local) / exec (containerised) —
    /// the same vars as `up`, so a compose file that interpolates env resolves
    /// to the same project definition at teardown. Not part of the argv.
    std::vector<std::pair<std::string, std::string>> env;
    bool volumes = true;        ///< --volumes
    bool remove_images = false; ///< --rmi all
};

/// Build the argv following `docker compose` for an `up` command, i.e. starting
/// at `--project-name`. Order: `--project-name <p>`, every `-f <file>`, every
/// `--profile <name>` (a top-level compose flag, so before the subcommand),
/// `up`, `-d`, then the conditional flags (`--build`, `--pull always`, `--wait
/// --wait-timeout <n>`). The env is NOT part of the argv.
std::vector<std::string> build_compose_up_args(const ComposeUpCommand& command);

/// Build the argv following `docker compose` for a `down` command: `--project-name
/// <p>`, every `--profile <name>`, `down`, then the conditional `--volumes` /
/// `--rmi all`. We pass `--rmi all` (compose v2 requires an argument; rust pushes
/// a bare `--rmi`, which is a v1-ism — we deviate for correctness).
std::vector<std::string> build_compose_down_args(const ComposeDownCommand& command);

/// Single-quote a token for /bin/sh, escaping embedded single quotes
/// (`'` -> `'\''`). Used by the containerised client to wrap env-prefixed
/// compose invocations in `/bin/sh -c "..."`. Pure — unit-testable.
std::string shell_quote(const std::string& s);

/// A `KEY='value'` env assignment for /bin/sh (the value shell-quoted; KEY is
/// emitted verbatim — library-controlled, not validated or quoted).
std::string shell_quote_assignment(const std::string& key, const std::string& value);

/// Assemble the one-line /bin/sh script the containerised client execs when
/// env vars must prefix a compose invocation:
/// `KEY1='v1' KEY2='v2' 'argv0' 'argv1' ...` — every value and argv token
/// shell-quoted. With an empty `env` it degrades to just the quoted argv.
/// Pure — unit-testable without a daemon.
std::string build_env_wrapped_script(const std::vector<std::string>& argv,
                                     const std::vector<std::pair<std::string, std::string>>& env);

} // namespace testcontainers::compose
