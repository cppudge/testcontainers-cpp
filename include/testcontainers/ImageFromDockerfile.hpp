#pragma once

#include <string>
#include <utility>
#include <vector>

namespace testcontainers {

/// A reusable, copyable description of an image to build from a Dockerfile —
/// either an inline Dockerfile string plus in-memory context files, or a host
/// directory used as the build context.
///
/// The `with_*` builders mutate in place and return `*this` by reference, so a
/// named config can be configured incrementally — no consume-self, no
/// use-after-move (mirrors GenericImage).
///
/// Public on purpose: the header pulls in only std types. The filesystem walk
/// and tar packing happen in the .cpp so no libarchive/Boost leaks here.
class ImageFromDockerfile {
public:
    /// One in-memory file added to the build context.
    struct File {
        std::string path; ///< path within the context (e.g. "Dockerfile", "app/x.txt")
        std::string content;
    };

    /// Build from an inline Dockerfile. The context is this Dockerfile plus any
    /// files added with with_file(). If `image_tag` is empty a unique tag like
    /// "tc-build-<hex>:latest" is generated at build() time.
    static ImageFromDockerfile from_content(std::string dockerfile_content,
                                            std::string image_tag = "");

    /// Build using a host directory as the context. `dockerfile` is the path to
    /// the Dockerfile relative to `context_dir` (default "Dockerfile").
    static ImageFromDockerfile from_path(std::string context_dir,
                                         std::string dockerfile = "Dockerfile",
                                         std::string image_tag = "");

    // --- In-place builders (single overload; chain on lvalues and temporaries) ---

    /// Add an in-memory file to the context (e.g. an extra file COPYed by the
    /// Dockerfile). `path_in_context` uses '/' separators.
    ImageFromDockerfile& with_file(std::string path_in_context, std::string content) {
        files_.push_back(File{std::move(path_in_context), std::move(content)});
        return *this;
    }

    ImageFromDockerfile& with_build_arg(std::string key, std::string value) {
        build_args_.emplace_back(std::move(key), std::move(value));
        return *this;
    }

    ImageFromDockerfile& with_target(std::string stage) {
        target_ = std::move(stage);
        return *this;
    }

    ImageFromDockerfile& with_no_cache(bool no_cache = true) {
        no_cache_ = no_cache;
        return *this;
    }

    ImageFromDockerfile& with_pull(bool pull = true) {
        pull_ = pull;
        return *this;
    }

    ImageFromDockerfile& with_tag(std::string image_tag) {
        image_tag_ = std::move(image_tag);
        return *this;
    }

    // --- Getters ---

    const std::string& image_tag() const noexcept { return image_tag_; }
    const std::string& dockerfile() const noexcept { return dockerfile_; }
    const std::string& dockerfile_content() const noexcept { return dockerfile_content_; }
    const std::string& context_dir() const noexcept { return context_dir_; }
    bool from_host_path() const noexcept { return from_path_; }
    const std::vector<File>& files() const noexcept { return files_; }
    const std::vector<std::pair<std::string, std::string>>& build_args() const noexcept {
        return build_args_;
    }
    const std::string& target() const noexcept { return target_; }
    bool no_cache() const noexcept { return no_cache_; }
    bool pull() const noexcept { return pull_; }

    /// Build the image and return its resolved reference ("repo:tag"), suitable
    /// for GenericImage::from_reference(...). Throws DockerError on failure.
    std::string build() const;

private:
    ImageFromDockerfile() = default;

    std::string image_tag_;        ///< explicit tag, or "" to auto-generate at build()
    std::string dockerfile_ = "Dockerfile"; ///< Dockerfile path within the context
    std::string dockerfile_content_;        ///< inline Dockerfile body (from_content)
    std::string context_dir_;               ///< host context directory (from_path)
    bool from_path_ = false;                ///< true when built from a host directory
    std::vector<File> files_;
    std::vector<std::pair<std::string, std::string>> build_args_;
    std::string target_;
    bool no_cache_ = false;
    bool pull_ = false;
};

} // namespace testcontainers
