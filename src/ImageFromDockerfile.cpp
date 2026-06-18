#include "testcontainers/ImageFromDockerfile.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "docker/Tar.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/BuildOptions.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers {

namespace {

/// Generate a unique-ish build tag like "tc-build-1a2b3c4d5e6f7a8b:latest"
/// (16 hex chars). Mirrors the random_device + hex idiom in Network.cpp.
std::string random_build_tag() {
    static constexpr char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    std::array<char, 16> buf{};
    for (char& c : buf) {
        c = hex[dist(rd)];
    }
    return "tc-build-" + std::string(buf.data(), buf.size()) + ":latest";
}

/// Read the whole host file into a string (binary), or throw DockerError.
std::string read_host_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw DockerError("ImageFromDockerfile: cannot open context file '" + path.string() + "'");
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw DockerError("ImageFromDockerfile: failed reading context file '" + path.string() +
                          "'");
    }
    return data;
}

} // namespace

ImageFromDockerfile ImageFromDockerfile::from_content(std::string dockerfile_content,
                                                      std::string image_tag) {
    ImageFromDockerfile img;
    img.dockerfile_content_ = std::move(dockerfile_content);
    img.image_tag_ = std::move(image_tag);
    img.from_path_ = false;
    return img;
}

ImageFromDockerfile ImageFromDockerfile::from_path(std::string context_dir, std::string dockerfile,
                                                   std::string image_tag) {
    ImageFromDockerfile img;
    img.context_dir_ = std::move(context_dir);
    img.dockerfile_ = std::move(dockerfile);
    img.image_tag_ = std::move(image_tag);
    img.from_path_ = true;
    return img;
}

std::string ImageFromDockerfile::build() const {
    // Resolve the tag: auto-generate a unique "tc-build-<hex>:latest" when none
    // was set, otherwise default a tag-less reference to ":latest" so the result
    // round-trips through GenericImage::from_reference / split_image.
    std::string tag = image_tag_;
    if (tag.empty()) {
        tag = random_build_tag();
    } else if (tag.find(':') == std::string::npos) {
        tag += ":latest";
    }

    // Assemble the build context.
    std::vector<docker::TarFile> files;
    if (from_path_) {
        // Walk the host context directory, adding every regular file with a
        // context-relative, '/'-separated name. (No .dockerignore handling.)
        const std::filesystem::path root(context_dir_);
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string rel = entry.path().lexically_relative(root).generic_string();
            files.push_back(docker::TarFile{rel, read_host_file(entry.path())});
        }
    } else {
        // Inline Dockerfile: place it under the BuildOptions.dockerfile name.
        files.push_back(docker::TarFile{dockerfile_, dockerfile_content_});
    }
    // In-memory files apply to both modes (e.g. extra files COPYed by the build).
    for (const File& f : files_) {
        files.push_back(docker::TarFile{f.path, f.content});
    }

    const std::string context_tar = docker::build_context_tar(files);

    docker::BuildOptions options;
    options.tag = tag;
    options.dockerfile = dockerfile_;
    options.build_args = build_args_;
    options.target = target_;
    options.no_cache = no_cache_;
    options.pull = pull_;

    DockerClient client = DockerClient::from_environment();
    client.build_image(context_tar, options);

    return tag;
}

} // namespace testcontainers
