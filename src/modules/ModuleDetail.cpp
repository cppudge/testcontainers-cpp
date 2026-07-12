#include "ModuleDetail.hpp"

#include <optional>
#include <string_view>
#include <utility>

#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers::modules::detail {

namespace {

/// The extensions the postgres and mysql/mariadb entrypoints execute;
/// anything else they SKIP silently — a green start with an empty schema —
/// so staging refuses unknown ones up front. Entrypoints with a different
/// executed set pass their own whitelist to the 4-argument overloads.
const std::vector<std::string_view>& default_init_extensions() {
    static const std::vector<std::string_view> extensions{".sql", ".sql.gz", ".sql.xz", ".sql.zst",
                                                          ".sh"};
    return extensions;
}

bool has_known_init_extension(std::string_view name, const std::vector<std::string_view>& allowed) {
    for (const std::string_view ext : allowed) {
        if (name.size() > ext.size() && name.ends_with(ext)) {
            return true;
        }
    }
    return false;
}

void check_init_script(const std::string& name, std::size_t index, const char* entrypoint_name,
                       const std::vector<std::string_view>& allowed) {
    if (!has_known_init_extension(name, allowed)) {
        std::string allowed_list;
        for (const std::string_view ext : allowed) {
            if (!allowed_list.empty()) {
                allowed_list += ", ";
            }
            allowed_list += ext;
        }
        throw Error("init script '" + name + "' has an extension " + entrypoint_name +
                    " would silently ignore; use one of: " + allowed_list);
    }
    if (index >= 10000) {
        throw Error("too many init scripts (the 0000- ordering prefix is four digits)");
    }
}

std::string init_target(std::size_t index, const std::string& name) {
    return "/docker-entrypoint-initdb.d/" + zero_pad4(index) + "-" + name;
}

CopyToContainer shipped_executable_if_sh(const std::string& name, CopyToContainer copy) {
    if (name.ends_with(".sh")) {
        // Executable, so the entrypoint runs it as a standalone script; a
        // non-executable .sh is SOURCED into the entrypoint's shell instead
        // (where a stray `exit` kills the boot).
        copy.with_mode(0755);
    }
    return copy;
}

} // namespace

std::string zero_pad4(std::size_t value) {
    std::string digits = std::to_string(value);
    if (digits.size() < 4) {
        digits.insert(0, 4 - digits.size(), '0');
    }
    return digits;
}

CopyToContainer stage_init_script(std::filesystem::path host_path, std::size_t index,
                                  const char* entrypoint_name) {
    return stage_init_script(std::move(host_path), index, entrypoint_name,
                             default_init_extensions());
}

CopyToContainer stage_init_script(const std::string& name, std::string content, std::size_t index,
                                  const char* entrypoint_name) {
    return stage_init_script(name, std::move(content), index, entrypoint_name,
                             default_init_extensions());
}

CopyToContainer stage_init_script(std::filesystem::path host_path, std::size_t index,
                                  const char* entrypoint_name,
                                  const std::vector<std::string_view>& allowed_extensions) {
    const std::string name = host_path.filename().string();
    check_init_script(name, index, entrypoint_name, allowed_extensions);
    return shipped_executable_if_sh(
        name, CopyToContainer::host_file(std::move(host_path), init_target(index, name)));
}

CopyToContainer stage_init_script(const std::string& name, std::string content, std::size_t index,
                                  const char* entrypoint_name,
                                  const std::vector<std::string_view>& allowed_extensions) {
    if (name.empty() || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        throw Error("init script name '" + name + "' must be a bare file name");
    }
    check_init_script(name, index, entrypoint_name, allowed_extensions);
    return shipped_executable_if_sh(
        name, CopyToContainer::content(std::move(content), init_target(index, name)));
}

ExecResult exec_or_throw(DockerClient& client, const std::string& id,
                         const std::vector<std::string>& cmd, const std::string& what) {
    ExecResult res = client.exec(id, cmd);
    if (res.exit_code != 0) {
        throw DockerError(what + " failed (exit " + std::to_string(res.exit_code) +
                              "): " + (res.stderr_data.empty() ? res.stdout_data : res.stderr_data),
                          std::nullopt, id);
    }
    return res;
}

} // namespace testcontainers::modules::detail
