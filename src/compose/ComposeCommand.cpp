#include "compose/ComposeCommand.hpp"

#include <string>

namespace testcontainers::compose {

std::vector<std::string> build_compose_up_args(const ComposeUpCommand& command) {
    std::vector<std::string> args;
    args.emplace_back("--project-name");
    args.push_back(command.project_name);

    for (const std::string& file : command.files) {
        args.emplace_back("-f");
        args.push_back(file);
    }

    args.emplace_back("up");
    args.emplace_back("-d");

    if (command.build) {
        args.emplace_back("--build");
    }
    if (command.pull) {
        args.emplace_back("--pull");
        args.emplace_back("always");
    }
    if (command.wait) {
        args.emplace_back("--wait");
        args.emplace_back("--wait-timeout");
        args.push_back(std::to_string(command.wait_timeout_secs));
    }

    return args;
}

std::vector<std::string> build_compose_down_args(const ComposeDownCommand& command) {
    std::vector<std::string> args;
    args.emplace_back("--project-name");
    args.push_back(command.project_name);
    args.emplace_back("down");

    if (command.volumes) {
        args.emplace_back("--volumes");
    }
    if (command.remove_images) {
        // Compose v2's `--rmi` requires an argument ("all" | "local"); a bare
        // `--rmi` (as rust pushes) errors. Remove all images for the project.
        args.emplace_back("--rmi");
        args.emplace_back("all");
    }

    return args;
}

} // namespace testcontainers::compose
