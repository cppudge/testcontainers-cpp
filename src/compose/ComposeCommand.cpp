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

    for (const std::string& profile : command.profiles) {
        args.emplace_back("--profile");
        args.push_back(profile);
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
    for (const auto& [service, instances] : command.scales) {
        args.emplace_back("--scale");
        args.push_back(service + "=" + std::to_string(instances));
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

    for (const std::string& profile : command.profiles) {
        args.emplace_back("--profile");
        args.push_back(profile);
    }

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

std::string shell_quote(const std::string& s) {
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

std::string shell_quote_assignment(const std::string& key, const std::string& value) {
    return key + "=" + shell_quote(value);
}

std::string build_env_wrapped_script(const std::vector<std::string>& argv,
                                     const std::vector<std::pair<std::string, std::string>>& env) {
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
    return script;
}

} // namespace testcontainers::compose
