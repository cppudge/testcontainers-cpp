#include "testcontainers/GenericBuildableImage.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "FileRead.hpp"
#include "Reaper.hpp"
#include "docker/DockerIgnore.hpp"
#include "docker/Tar.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/BuildOptions.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers {

namespace {

/// Append descriptors for every regular file under `src` (a host directory
/// mapped to `target` within the context) — path + mode only, bytes are read
/// at upload time. A `.dockerignore` at the ROOT of `src` filters the walk
/// (docker-build patternmatcher semantics, paths relative to `src`). For a
/// source mapped to the CONTEXT ROOT (empty `target`), `.dockerignore` and
/// `Dockerfile` at its root are exempt from the filtering, exactly as
/// `docker build` ships them regardless of the ignore rules — except that a
/// walked-up `Dockerfile` is dropped entirely when the caller already added
/// one explicitly (`skip_dockerfile`): tar extraction is last-wins, and the
/// with_dockerfile* entry must not be shadowed by a stray host file.
void append_dir_context(std::vector<docker::TarFile>& files, const std::filesystem::path& src,
                        const std::string& target, int mode, bool skip_dockerfile) {
    std::vector<docker::IgnorePattern> patterns;
    const std::string ignore_text = detail::read_file((src / ".dockerignore").string());
    if (!ignore_text.empty()) {
        patterns = docker::parse_dockerignore(ignore_text);
    }

    const bool root_mapped = target.empty();
    const std::filesystem::path base(target);
    const std::size_t first_appended = files.size();
    try {
        for (const auto& it : std::filesystem::recursive_directory_iterator(src)) {
            if (!it.is_regular_file()) {
                continue;
            }
            // u8string keeps entry names UTF-8 regardless of the host code
            // page (same reasoning as the tar walk in docker/Tar.cpp;
            // generic_string would re-encode through the ANSI page on
            // Windows).
            const std::filesystem::path rel_path = it.path().lexically_relative(src);
            const std::u8string rel8 = rel_path.generic_u8string();
            const std::string rel(rel8.begin(), rel8.end());
            if (root_mapped && rel == "Dockerfile" && skip_dockerfile) {
                continue;
            }
            const bool always_shipped =
                root_mapped && (rel == ".dockerignore" || rel == "Dockerfile");
            if (!always_shipped && !patterns.empty() &&
                docker::dockerignore_excludes(patterns, rel)) {
                continue;
            }
            docker::TarFile file;
            const std::u8string name8 = (base / rel_path).generic_u8string();
            file.name = std::string(name8.begin(), name8.end());
            file.path = it.path();
            file.mode = mode;
            files.push_back(std::move(file));
        }
    } catch (const std::filesystem::filesystem_error& e) {
        throw DockerError("build context: cannot walk host directory '" + src.string() +
                          "': " + e.what());
    }
    // Directory iteration order is unspecified; sort this source's expansion
    // so the context tar — and any digest of it — is deterministic across
    // rebuilds (mirrors the copy-to tar walk).
    std::sort(files.begin() + static_cast<std::ptrdiff_t>(first_appended), files.end(),
              [](const docker::TarFile& a, const docker::TarFile& b) { return a.name < b.name; });
}

} // namespace

GenericImage GenericBuildableImage::build() const {
    // Expand the build-context entries into tar-file DESCRIPTORS: an in-memory
    // entry keeps its bytes, but a host file contributes only its path (read
    // in blocks while the upload streams). A host directory is walked
    // recursively (each regular file lands at "<target>/<relpath>",
    // '/'-separated), honoring a .dockerignore at its root.
    //
    // An explicit "Dockerfile" entry (with_dockerfile* / with_data) takes
    // precedence: a root-mapped directory that happens to contain its own
    // Dockerfile must not shadow it (see append_dir_context).
    const bool explicit_dockerfile =
        std::any_of(context_.begin(), context_.end(), [](const CopyToContainer& entry) {
            return !entry.is_dir() && entry.target() == "Dockerfile";
        });

    std::vector<docker::TarFile> files;
    for (const CopyToContainer& entry : context_) {
        if (!entry.is_file()) {
            docker::TarFile file;
            file.name = entry.target();
            file.body = entry.bytes();
            file.mode = entry.mode();
            files.push_back(std::move(file));
            continue;
        }

        const std::filesystem::path& src = entry.host_path();
        std::error_code ec;
        if (std::filesystem::is_directory(src, ec)) {
            append_dir_context(files, src, entry.target(), entry.mode(), explicit_dockerfile);
        } else {
            docker::TarFile file;
            file.name = entry.target();
            file.path = src;
            file.mode = entry.mode();
            files.push_back(std::move(file));
        }
    }

    docker::BuildOptions options;
    options.tag = descriptor();        // "<name>:<tag>"
    options.dockerfile = "Dockerfile"; // with_dockerfile* always target "Dockerfile"
    options.build_args = build_args_;
    options.target = target_;
    options.no_cache = no_cache_;
    options.pull = pull_;

    // The built image carries the same managed-by/session labels as containers,
    // so Ryuk removes it once this process exits (crash-safe cleanup). With the
    // reaper disabled the session label is omitted and the image persists.
    for (const auto& label : detail::testcontainers_labels()) {
        options.labels.push_back(label);
    }

    // The labels are only reaped if a reaper is actually watching the session —
    // boot it BEFORE building, like run() does before creating a container
    // (no-op when Ryuk is disabled or on a Windows-containers daemon).
    detail::Reaper::instance().ensure_started();

    // Stream the context: blocks leave as they are produced, so the tar (and
    // the tree's file bodies) never sit in memory whole.
    DockerClient client = DockerClient::from_environment();
    client.build_image(
        [&files](const docker::ByteSink& sink) { docker::stream_context_tar(files, sink); },
        options, build_log_consumer_);

    // The image now exists locally tagged "<name>:<tag>"; hand back a runnable
    // GenericImage for it (start() finds it locally, no pull).
    return GenericImage(name_, tag_);
}

} // namespace testcontainers
