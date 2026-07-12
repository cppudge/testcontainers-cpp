#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/ExecResult.hpp"

namespace testcontainers {
class DockerClient;
}

// Module-layer plumbing shared across the technology wrappers. One copy on
// purpose: the initdb.d staging guards are load-bearing (a skipped script is
// a green start with an empty schema), and hook failures must all read the
// same way.

namespace testcontainers::modules::detail {

/// `value` as four zero-padded digits ("0007") — the ordering prefix for
/// staged files (initdb.d scripts, RabbitMQ definitions): the images apply
/// them in C-collation name order, so a padded registration index makes
/// registration order the effective order.
std::string zero_pad4(std::size_t value);

/// Validate and stage one /docker-entrypoint-initdb.d init script (the
/// contract the postgres AND mysql/mariadb images share): whitelist the
/// extensions the entrypoints execute (.sql, .sql.gz, .sql.xz, .sql.zst, .sh
/// — anything else is SKIPPED silently), prefix the container-side name with
/// the zero-padded registration `index`, and ship `.sh` executable (a
/// non-executable .sh is SOURCED into the entrypoint's shell, where a stray
/// `exit` kills the boot). Throws Error on an unknown extension or an index
/// past the 4-digit prefix; `entrypoint_name` names the offender in the
/// message ("the postgres entrypoint" / "the image's entrypoint").
CopyToContainer stage_init_script(std::filesystem::path host_path, std::size_t index,
                                  const char* entrypoint_name);

/// The in-memory variant: `name` must be a bare file name (no separators).
CopyToContainer stage_init_script(const std::string& name, std::string content, std::size_t index,
                                  const char* entrypoint_name);

/// The same staging with an explicit extension whitelist, for entrypoints
/// whose executed set differs from the postgres/mysql one (ClickHouse runs
/// .sql/.sql.gz/.sh only — its entrypoint ignores the xz/zst forms).
CopyToContainer stage_init_script(std::filesystem::path host_path, std::size_t index,
                                  const char* entrypoint_name,
                                  const std::vector<std::string_view>& allowed_extensions);

/// The in-memory variant of the explicit-whitelist staging.
CopyToContainer stage_init_script(const std::string& name, std::string content, std::size_t index,
                                  const char* entrypoint_name,
                                  const std::vector<std::string_view>& allowed_extensions);

/// Exec `cmd` in container `id` and return the result; a non-zero exit
/// throws DockerError "<what> failed (exit N): <stderr, else stdout>"
/// carrying `id` — the one shape every module hook failure reads as.
ExecResult exec_or_throw(DockerClient& client, const std::string& id,
                         const std::vector<std::string>& cmd, const std::string& what);

} // namespace testcontainers::modules::detail
