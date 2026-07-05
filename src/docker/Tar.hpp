#pragma once

#include <string>
#include <vector>

#include "testcontainers/CopyToContainer.hpp"

// In-memory tar (USTAR) builder for the copy-to-container endpoint. Docker's
// `PUT /containers/{id}/archive` takes a tar archive and extracts it into the
// directory named by the `path` query parameter. We always PUT with `path=/`
// and make each entry's name relative (see normalize_entry_name), so an entry
// named `tmp/foo.txt` lands at `/tmp/foo.txt`.
//
// Kept separate from DockerClient (and free of any Boost/HTTP dependency) so it
// can be unit-tested without a daemon: build a tar, read it back with libarchive
// and assert the entry fields.
namespace testcontainers::docker {

/// Strip a single leading '/' from a path (USTAR entry names are relative).
/// A path without a leading slash is returned unchanged.
std::string strip_leading_slash(const std::string& path);

/// Normalize a container-side copy target into a tar entry name. A Windows
/// drive-rooted target ("C:\dir\file" or "C:/dir/file") drops the drive prefix
/// and turns '\' separators into '/' — on a Windows engine, `path=/` extraction
/// is relative to `C:\`, so the result lands where the caller pointed. Then the
/// leading '/' is stripped. Backslashes in non-drive-rooted targets are left
/// alone (a valid character in Linux file names).
std::string normalize_entry_name(const std::string& target);

/// Build a tar archive (USTAR) for `source` and return the raw tar bytes.
///
/// For a host-file or in-memory-bytes source the archive holds ONE regular-file
/// entry: the entry name is `source.target()` normalized (see
/// normalize_entry_name), the size and mode are set, and the body is the host
/// file's bytes (when `source.is_file()`) or `source.bytes()`.
///
/// For a directory source (`source.is_dir()`) the archive holds a directory
/// entry for the normalized target and each of its path components (so nothing
/// needs to pre-exist in the container), plus one entry per directory and
/// regular file under `source.host_path()`, in sorted order. Files carry
/// `source.mode()`; directories are 0755. Directory symlinks are not descended
/// into; non-regular files (sockets, FIFOs, dangling symlinks) are skipped.
///
/// Throws DockerError if a host file or directory cannot be read or the
/// archive cannot be produced.
std::string build_tar(const CopyToContainer& source);

/// One file to place into a build-context tar.
struct TarFile {
    std::string name; ///< path within the context (e.g. "Dockerfile", "app/x.txt")
    std::string body; ///< file contents (may be binary)
    int mode = 0644;  ///< octal regular-file mode
};

/// Build a USTAR archive holding all `files` as regular-file entries (names used
/// verbatim, leading '/' stripped). Returns the raw tar bytes. Throws DockerError
/// on failure. This is the build-context body for `POST /build`.
std::string build_context_tar(const std::vector<TarFile>& files);

/// One entry extracted from a tar archive.
struct TarEntry {
    std::string name;             ///< entry pathname as stored in the archive
    std::string body;             ///< file contents (empty for non-regular entries)
    bool is_regular_file = false; ///< true for AE_IFREG entries (files), else dirs/links
    int mode = 0;                 ///< permission bits (e.g. 0644)
};

/// Extract every entry from an in-memory tar archive (the body returned by
/// `GET /containers/{id}/archive`). Throws DockerError if the archive cannot be
/// opened or parsed.
std::vector<TarEntry> extract_tar(const std::string& tar_bytes);

} // namespace testcontainers::docker
