#include "compose/ComposeClients.hpp"

#include "compose/Process.hpp"

#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/ExecResult.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace testcontainers::compose {

namespace {

/// The label we stamp on the long-lived containerised cli container so it can be
/// distinguished as ours (matches the convention used elsewhere in the library).
constexpr const char* kManagedByLabel = "org.testcontainers.managed-by";

/// The compose v2 subcommand prefix every invocation shares.
std::vector<std::string> docker_compose_prefix() { return {"docker", "compose"}; }

} // namespace

// ===== Local client ========================================================
//
// Shells out to the host `docker compose` CLI via run_process. THIS IS THE ONLY
// place in the library that invokes the host docker CLI — the documented,
// compose-only exception to the project's pure-Engine-API rule.
namespace {

class LocalComposeClient : public IComposeClient {
public:
    explicit LocalComposeClient(std::vector<std::string> compose_files)
        : compose_files_(std::move(compose_files)),
          project_directory_(extract_project_directory(compose_files_)) {}

    void up(const ComposeUpCommand& command) override {
        // Build the up command over the HOST file paths (not the abstract files
        // the caller may have set — the local client owns the real paths).
        ComposeUpCommand local = command;
        local.files = compose_files_;

        std::vector<std::string> argv = docker_compose_prefix();
        argv.emplace_back("--project-directory");
        argv.push_back(project_directory_);
        const std::vector<std::string> up_args = build_compose_up_args(local);
        argv.insert(argv.end(), up_args.begin(), up_args.end());

        const ProcessResult result =
            run_process(argv, /*working_dir*/ std::nullopt, command.env);
        if (result.exit_code != 0) {
            throw DockerError("docker compose up exited with status " +
                              std::to_string(result.exit_code) + ":\n" + result.output);
        }
    }

    void down(const ComposeDownCommand& command) override {
        std::vector<std::string> argv = docker_compose_prefix();
        argv.emplace_back("--project-directory");
        argv.push_back(project_directory_);
        // Carry the -f files so compose resolves the same project definition.
        for (const std::string& file : compose_files_) {
            argv.emplace_back("-f");
            argv.push_back(file);
        }
        const std::vector<std::string> down_args = build_compose_down_args(command);
        argv.insert(argv.end(), down_args.begin(), down_args.end());

        const ProcessResult result = run_process(argv);
        if (result.exit_code != 0) {
            throw DockerError("docker compose down exited with status " +
                              std::to_string(result.exit_code) + ":\n" + result.output);
        }
    }

private:
    /// The compose `--project-directory` is the parent of the first compose file
    /// (mirrors rust's LocalComposeCli::extract_current_dir), defaulting to ".".
    static std::string extract_project_directory(const std::vector<std::string>& files) {
        if (files.empty()) {
            return ".";
        }
        std::filesystem::path parent = std::filesystem::path(files.front()).parent_path();
        if (parent.empty()) {
            return ".";
        }
        return parent.string();
    }

    std::vector<std::string> compose_files_; ///< absolute host paths
    std::string project_directory_;          ///< --project-directory
};

} // namespace

// ===== Containerised client ================================================
//
// Runs `docker compose` inside a LONG-LIVED `docker:cli` container (entrypoint
// /bin/sh, cmd `-c sleep infinity`) with the host docker socket bind-mounted.
// Each compose file is copied in as /docker-compose-<i>.yml. up/down are
// DockerClient::exec calls; the container is force-removed on teardown.
namespace {

class ContainerisedComposeClient : public IComposeClient {
public:
    ContainerisedComposeClient(const std::vector<std::string>& compose_files,
                               const std::string& compose_image)
        : client_(DockerClient::from_environment()) {
        // Compute the in-container file names (/docker-compose-<i>.yml).
        files_in_container_.reserve(compose_files.size());
        for (std::size_t i = 0; i < compose_files.size(); ++i) {
            files_in_container_.push_back("/docker-compose-" + std::to_string(i) + ".yml");
        }

        // Start the long-lived cli container. The in-container default
        // DOCKER_HOST is already this unix socket, so no DOCKER_HOST env needed.
        CreateContainerSpec spec;
        spec.image = compose_image;
        spec.entrypoint = {"/bin/sh"};
        spec.cmd = {"-c", "sleep infinity"};
        spec.mounts = {Mount::bind("/var/run/docker.sock", "/var/run/docker.sock")};
        spec.labels = {{kManagedByLabel, "testcontainers"}};
        spec.auto_remove = false;

        cli_id_ = client_.create_container(spec);
        try {
            client_.start_container(cli_id_);
            // Copy each compose file in (the parent dir "/" already exists).
            for (std::size_t i = 0; i < compose_files.size(); ++i) {
                client_.copy_to_container(
                    cli_id_, CopyToContainer::host_file(compose_files[i], files_in_container_[i]));
            }
        } catch (...) {
            // Best-effort cleanup if start/copy failed mid-construction.
            try {
                client_.remove_container(cli_id_, /*force*/ true, /*remove_volumes*/ true);
            } catch (...) {
            }
            throw;
        }
    }

    ~ContainerisedComposeClient() override {
        // The long-lived cli container is owned by this client; force-remove it.
        try {
            client_.remove_container(cli_id_, /*force*/ true, /*remove_volumes*/ true);
        } catch (...) {
        }
    }

    void up(const ComposeUpCommand& command) override {
        // Build over the in-container file paths.
        ComposeUpCommand cmd = command;
        cmd.files = files_in_container_;

        std::vector<std::string> exec_cmd = docker_compose_prefix();
        const std::vector<std::string> up_args = build_compose_up_args(cmd);
        exec_cmd.insert(exec_cmd.end(), up_args.begin(), up_args.end());

        // Pass env through to compose by prefixing `KEY=VALUE` env exports via
        // the shell (DockerClient::exec has no env field). We wrap in `/bin/sh
        // -c` so the env assignments apply to the compose invocation.
        const ExecResult result = exec_compose(exec_cmd, command.env);
        if (result.exit_code != 0) {
            throw DockerError("docker compose up exited with status " +
                              std::to_string(result.exit_code) + ":\n" + result.stdout_data +
                              result.stderr_data);
        }
    }

    void down(const ComposeDownCommand& command) override {
        std::vector<std::string> exec_cmd = docker_compose_prefix();
        const std::vector<std::string> down_args = build_compose_down_args(command);
        exec_cmd.insert(exec_cmd.end(), down_args.begin(), down_args.end());

        const ExecResult result = exec_compose(exec_cmd, {});
        if (result.exit_code != 0) {
            throw DockerError("docker compose down exited with status " +
                              std::to_string(result.exit_code) + ":\n" + result.stdout_data +
                              result.stderr_data);
        }
    }

private:
    /// Exec a `docker compose ...` argv inside the cli container. When `env` is
    /// non-empty we wrap the call in `/bin/sh -c "KEY=VALUE ... docker compose
    /// ..."` so the env vars are visible to compose (the exec API here has no
    /// dedicated env field). With no env we exec the argv directly.
    ExecResult exec_compose(const std::vector<std::string>& argv,
                            const std::vector<std::pair<std::string, std::string>>& env) {
        if (env.empty()) {
            return client_.exec(cli_id_, argv);
        }
        std::string script;
        for (const auto& [key, value] : env) {
            script += shell_quote_assignment(key, value);
            script += ' ';
        }
        for (std::size_t i = 0; i < argv.size(); ++i) {
            if (i != 0) {
                script.push_back(' ');
            }
            script += shell_quote(argv[i]);
        }
        return client_.exec(cli_id_, {"/bin/sh", "-c", script});
    }

    /// Single-quote a token for /bin/sh, escaping embedded single quotes.
    static std::string shell_quote(const std::string& s) {
        std::string out = "'";
        for (const char c : s) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out.push_back(c);
            }
        }
        out.push_back('\'');
        return out;
    }

    /// `KEY='value'` env assignment for the shell.
    static std::string shell_quote_assignment(const std::string& key, const std::string& value) {
        return key + "=" + shell_quote(value);
    }

    DockerClient client_;
    std::string cli_id_;                          ///< the long-lived cli container id
    std::vector<std::string> files_in_container_; ///< /docker-compose-<i>.yml paths
};

/// Probe whether the host `docker compose` CLI is available (Auto resolution):
/// `docker compose version` exiting 0 means we can use the local client.
bool host_docker_compose_available() {
    try {
        const ProcessResult result = run_process({"docker", "compose", "version"});
        return result.exit_code == 0;
    } catch (...) {
        return false;
    }
}

} // namespace

std::unique_ptr<IComposeClient> make_compose_client(ClientKind kind,
                                                    const std::vector<std::string>& compose_files,
                                                    const std::string& compose_image) {
    switch (kind) {
    case ClientKind::Local:
        return std::make_unique<LocalComposeClient>(compose_files);
    case ClientKind::Containerised:
        return std::make_unique<ContainerisedComposeClient>(compose_files, compose_image);
    case ClientKind::Auto:
        if (host_docker_compose_available()) {
            return std::make_unique<LocalComposeClient>(compose_files);
        }
        return std::make_unique<ContainerisedComposeClient>(compose_files, compose_image);
    }
    // Unreachable; keep the compiler happy.
    return std::make_unique<LocalComposeClient>(compose_files);
}

} // namespace testcontainers::compose
