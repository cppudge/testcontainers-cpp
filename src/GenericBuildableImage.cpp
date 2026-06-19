#include "testcontainers/GenericBuildableImage.hpp"

#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "docker/Tar.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/BuildOptions.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers {

namespace {

/// Read the whole host file into a string (binary), or throw DockerError.
std::string read_host_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw DockerError("GenericBuildableImage: cannot open context file '" + path.string() +
                          "'");
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw DockerError("GenericBuildableImage: failed reading context file '" + path.string() +
                          "'");
    }
    return data;
}

} // namespace

GenericImage GenericBuildableImage::build() const {
    // Expand the build-context entries into tar files. An in-memory entry maps to
    // one tar file at its target; a host-file entry is read as a single file, or,
    // when it points to a directory, walked recursively (each regular file lands
    // at "<target>/<relpath>", '/'-separated). No .dockerignore handling.
    std::vector<docker::TarFile> files;
    for (const CopyToContainer& entry : context_) {
        if (!entry.is_file()) {
            files.push_back(docker::TarFile{entry.target(), entry.bytes(), entry.mode()});
            continue;
        }

        const std::filesystem::path src(entry.host_path());
        std::error_code ec;
        if (std::filesystem::is_directory(src, ec)) {
            const std::filesystem::path base(entry.target());
            for (const auto& it : std::filesystem::recursive_directory_iterator(src)) {
                if (!it.is_regular_file()) {
                    continue;
                }
                const std::string rel = it.path().lexically_relative(src).generic_string();
                const std::string name = (base / rel).generic_string();
                files.push_back(docker::TarFile{name, read_host_file(it.path()), entry.mode()});
            }
        } else {
            files.push_back(docker::TarFile{entry.target(), read_host_file(src), entry.mode()});
        }
    }

    const std::string context_tar = docker::build_context_tar(files);

    docker::BuildOptions options;
    options.tag = descriptor();      // "<name>:<tag>"
    options.dockerfile = "Dockerfile"; // with_dockerfile* always target "Dockerfile"
    options.build_args = build_args_;
    options.target = target_;
    options.no_cache = no_cache_;
    options.pull = pull_;

    DockerClient client = DockerClient::from_environment();
    client.build_image(context_tar, options);

    // The image now exists locally tagged "<name>:<tag>"; hand back a runnable
    // GenericImage for it (start() finds it locally, no pull).
    return GenericImage(name_, tag_);
}

} // namespace testcontainers
