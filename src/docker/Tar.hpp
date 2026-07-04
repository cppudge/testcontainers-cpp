#pragma once

#include <string>
#include <vector>

#include "testcontainers/CopyToContainer.hpp"

// In-memory tar (USTAR) builder for the copy-to-container endpoint. Docker's
// `PUT /containers/{id}/archive` takes a tar archive and extracts it into the
// directory named by the `path` query parameter. We always PUT with `path=/`
// and make each entry's name relative (leading '/' stripped), so an entry named
// `tmp/foo.txt` lands at `/tmp/foo.txt`.
//
// Kept separate from DockerClient (and free of any Boost/HTTP dependency) so it
// can be unit-tested without a daemon: build a tar, read it back with libarchive
// and assert the entry fields.
namespace testcontainers::docker {

/// Strip a single leading '/' from a path (USTAR entry names are relative).
/// A path without a leading slash is returned unchanged.
std::string strip_leading_slash(const std::string& path);

/// Build a tar archive (USTAR) holding ONE regular-file entry for `source`:
/// the entry name is `source.target()` with the leading '/' stripped, the size
/// and mode are set, and the body is the host file's bytes (when
/// `source.is_file()`) or `source.bytes()`. Returns the raw tar bytes.
///
/// Throws DockerError if a host file cannot be read or the archive cannot be
/// produced.
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
