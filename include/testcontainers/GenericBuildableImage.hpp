#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/GenericImage.hpp"

namespace testcontainers {

/// A reusable, copyable description of an image to BUILD from a Dockerfile and a
/// build context.
///
/// Construct it with a name and tag, add the Dockerfile (from a host path or an
/// inline string) plus any context files/data, then call `build()` to build the
/// image (tagged `<name>:<tag>`) and get back a runnable `GenericImage`:
///
/// ```cpp
/// GenericImage img = GenericBuildableImage("app", "v1")
///                        .with_dockerfile_string("FROM alpine:3.20\n...")
///                        .with_data("hi", "/hello.txt")
///                        .build();
/// Container c = img.with_wait(wait_for::exit()).start();
/// ```
///
/// The build context is a list of `CopyToContainer` entries (the same value type
/// used for copy-to-container): each is either a host file/dir (`host_file`) or
/// in-memory bytes (`content`), placed at a target path within the context. The
/// Dockerfile is simply the entry targeting `Dockerfile` (Docker's default).
///
/// The `with_*` builders mutate in place and return `*this` by reference, so a
/// named config can be configured incrementally — no consume-self, no
/// use-after-move (mirrors GenericImage).
///
/// Public on purpose: the header pulls in only std + value types. The filesystem
/// walk and tar packing happen in the .cpp so no libarchive/Boost leaks here.
class GenericBuildableImage {
public:
    /// Construct a buildable image with the given name and tag.
    explicit GenericBuildableImage(std::string_view name, std::string_view tag = "latest")
        : name_(name), tag_(tag) {}

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Add a Dockerfile from the host filesystem, placed at `Dockerfile` in the
    /// build context.
    GenericBuildableImage& with_dockerfile(std::filesystem::path source_path) {
        context_.push_back(CopyToContainer::host_file(std::move(source_path), "Dockerfile"));
        return *this;
    }

    /// Add a Dockerfile from an inline string, placed at `Dockerfile` in the
    /// build context. Useful for generating a Dockerfile programmatically or
    /// embedding one in a test.
    GenericBuildableImage& with_dockerfile_string(std::string content) {
        context_.push_back(CopyToContainer::content(std::move(content), "Dockerfile"));
        return *this;
    }

    /// Add a host file or directory to the build context at `target` (a directory
    /// is added recursively, each file under `target/<relpath>`). The Dockerfile
    /// must be named `Dockerfile` unless added via `with_dockerfile*`.
    GenericBuildableImage& with_file(std::filesystem::path source_path, std::string target) {
        context_.push_back(CopyToContainer::host_file(std::move(source_path), std::move(target)));
        return *this;
    }

    /// Add in-memory bytes to the build context at `target`. `data` may be binary.
    GenericBuildableImage& with_data(std::string data, std::string target) {
        context_.push_back(CopyToContainer::content(std::move(data), std::move(target)));
        return *this;
    }

    /// Add a build argument (`buildargs=`), e.g. `with_build_arg("VERSION", "1.2")`.
    GenericBuildableImage& with_build_arg(std::string key, std::string value) {
        build_args_.emplace_back(std::move(key), std::move(value));
        return *this;
    }

    /// Build only up to a named stage of a multi-stage Dockerfile (`target=`).
    GenericBuildableImage& with_target(std::string stage) {
        target_ = std::move(stage);
        return *this;
    }

    /// Disable the build cache (`nocache=`).
    GenericBuildableImage& with_no_cache(bool no_cache = true) {
        no_cache_ = no_cache;
        return *this;
    }

    /// Always attempt to pull a newer version of the base image (`pull=`).
    GenericBuildableImage& with_pull(bool pull = true) {
        pull_ = pull;
        return *this;
    }

    // --- Getters ---

    const std::string& name() const noexcept { return name_; }
    const std::string& tag() const noexcept { return tag_; }
    /// The image reference the build produces (`<name>:<tag>`).
    std::string descriptor() const { return name_ + ":" + tag_; }
    const std::vector<CopyToContainer>& build_context() const noexcept { return context_; }
    const std::vector<std::pair<std::string, std::string>>& build_args() const noexcept {
        return build_args_;
    }
    const std::string& target() const noexcept { return target_; }
    bool no_cache() const noexcept { return no_cache_; }
    bool pull() const noexcept { return pull_; }

    /// Build the image (tagged `<name>:<tag>`) from the configured Dockerfile and
    /// build context, and return a runnable `GenericImage` for it. Throws
    /// DockerError on failure (including a build error embedded in the daemon's
    /// streamed output, which Docker reports with an HTTP 200).
    GenericImage build() const;

private:
    std::string name_;
    std::string tag_;
    std::vector<CopyToContainer> context_; ///< build-context entries (Dockerfile + files/data)
    std::vector<std::pair<std::string, std::string>> build_args_;
    std::string target_;
    bool no_cache_ = false;
    bool pull_ = false;
};

} // namespace testcontainers
