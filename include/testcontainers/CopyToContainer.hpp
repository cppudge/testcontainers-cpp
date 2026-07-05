#pragma once

#include <filesystem>
#include <string>
#include <utility>

namespace testcontainers {

/// A request to copy a host file, in-memory bytes, or a host directory tree
/// into a container, mapping to a tar `PUT`ed to `PUT /containers/{id}/archive`.
///
/// Three sources: a host file (read at copy time), raw in-memory bytes (which
/// may be binary, including embedded NULs), or a host directory copied
/// recursively. Build one with a static factory, then optionally tune the file
/// mode with the chainable setter (same style as `Mount`/`Healthcheck`).
///
/// Container-side targets are '/'-separated absolute paths regardless of the
/// host OS. On a Windows-containers engine a drive-rooted target such as
/// `C:\dir\file` (or `C:/dir/file`) is also accepted and treated as
/// `/dir/file`, which the daemon extracts relative to `C:\`.
///
/// A plain, copyable value type (no Boost/Asio/libarchive leakage).
class CopyToContainer {
public:
    /// Copy a host file (`host_path` on the local filesystem) to `container_target`
    /// (an absolute path in the container, e.g. "/tmp/foo.txt"). The file is read
    /// when the copy runs. The target's parent directory must already exist in
    /// the container.
    static CopyToContainer host_file(std::filesystem::path host_path,
                                     std::string container_target) {
        CopyToContainer c;
        c.source_ = Source::HostFile;
        c.host_path_ = std::move(host_path);
        c.target_ = std::move(container_target);
        return c;
    }

    /// Copy in-memory bytes to `container_target`. `bytes` is taken verbatim and
    /// may be binary (embedded NULs are preserved).
    static CopyToContainer content(std::string bytes, std::string container_target) {
        CopyToContainer c;
        c.source_ = Source::Bytes;
        c.bytes_ = std::move(bytes);
        c.target_ = std::move(container_target);
        return c;
    }

    /// Copy a host directory recursively: everything under `host_dir` lands
    /// under `container_target` (an absolute directory path in the container,
    /// e.g. "/opt/data"), which is created — along with any missing intermediate
    /// directories — by the extraction. Empty directories are preserved. Regular
    /// files get `mode()` (default 0644); directories are always 0755. File
    /// symlinks are followed (their contents are copied); directory symlinks are
    /// not descended into. The tree is read when the copy runs.
    static CopyToContainer host_dir(std::filesystem::path host_dir, std::string container_target) {
        CopyToContainer c;
        c.source_ = Source::HostDir;
        c.host_path_ = std::move(host_dir);
        c.target_ = std::move(container_target);
        return c;
    }

    // --- In-place setters (single overload; chain on lvalues and temporaries) ---

    /// Set the octal file mode of created regular-file entries (e.g. `0644`).
    /// Defaults to `0644` when unset. For a directory source this applies to
    /// every file in the tree (directories are always 0755).
    CopyToContainer& with_mode(int mode) {
        mode_ = mode;
        return *this;
    }

    // --- Getters ---

    /// True if the source is a host file.
    bool is_file() const noexcept { return source_ == Source::HostFile; }
    /// True if the source is a host directory tree.
    bool is_dir() const noexcept { return source_ == Source::HostDir; }
    const std::filesystem::path& host_path() const noexcept { return host_path_; }
    const std::string& bytes() const noexcept { return bytes_; }
    const std::string& target() const noexcept { return target_; }
    int mode() const noexcept { return mode_; }

private:
    enum class Source { HostFile, Bytes, HostDir };

    Source source_ = Source::Bytes;
    std::filesystem::path host_path_; ///< set for HostFile/HostDir (host filesystem path)
    std::string bytes_;               ///< set for Bytes (may be binary)
    std::string target_;              ///< absolute path inside the container
    int mode_ = 0644;                 ///< octal regular-file mode
};

} // namespace testcontainers
