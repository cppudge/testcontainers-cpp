#pragma once

#include <memory>
#include <string>
#include <vector>

#include "compose/ComposeCommand.hpp"

// The internal compose-client abstraction (mirrors rust's `ComposeInterface`
// trait + `ComposeClient` enum). Three implementations live in the .cpp:
//   - LocalComposeClient        — shells out to the host `docker compose` CLI.
//   - ContainerisedComposeClient — `docker compose` inside a long-lived
//                                  `docker:cli` container, driven via exec.
// and a factory `make_compose_client` that resolves the Auto kind.
//
// DockerClient / Boost usage stays in the .cpp; this header is std-only so the
// public DockerComposeContainer header never sees it.

namespace testcontainers::compose {

/// The selected compose client implementation. Mirrors rust's factory naming
/// (`with_local_client` / `with_containerised_client` / `with_auto_client`).
enum class ClientKind {
    Local,         ///< host `docker compose` CLI (the default)
    Containerised, ///< `docker compose` inside a long-lived container
    Auto,          ///< probe local; fall back to containerised
};

/// Internal interface every compose client implements. `up` / `down` THROW
/// DockerError on a non-zero compose exit, embedding the command's output.
class IComposeClient {
public:
    virtual ~IComposeClient() = default;

    /// Bring the project up (compose `up -d ...`). Throws on a non-zero exit.
    virtual void up(const ComposeUpCommand& command) = 0;

    /// Tear the project down (compose `down ...`). Throws on a non-zero exit.
    virtual void down(const ComposeDownCommand& command) = 0;
};

/// Resolve `kind` into a concrete client.
///   - Local: a LocalComposeClient over the host paths in `compose_files`; its
///     working dir is the parent of the first file (compose `--project-directory`).
///   - Containerised: starts/owns a long-lived `compose_image` container with the
///     docker socket bind-mounted and each file copied to `/docker-compose-<i>.yml`.
///   - Auto: probe `docker compose version` via run_process; exit 0 ⇒ Local, else
///     Containerised.
/// `compose_files` are absolute host paths. Throws DockerError on failure to
/// start the containerised client (e.g. image pull / start).
std::unique_ptr<IComposeClient> make_compose_client(ClientKind kind,
                                                    const std::vector<std::string>& compose_files,
                                                    const std::string& compose_image);

} // namespace testcontainers::compose
