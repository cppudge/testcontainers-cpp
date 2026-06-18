#pragma once

#include <string>

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

} // namespace testcontainers::docker
