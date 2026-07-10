#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "testcontainers/CopyToContainer.hpp"

// Streaming tar writer/reader for the archive endpoints. Docker's
// `PUT /containers/{id}/archive` takes a tar archive and extracts it into the
// directory named by the `path` query parameter. We always PUT with `path=/`
// and make each entry's name relative (see normalize_entry_name), so an entry
// named `tmp/foo.txt` lands at `/tmp/foo.txt`.
//
// Archives are produced in the pax "restricted" flavor: entries are plain
// USTAR until a field does not fit (a path over 100 chars, a file over 8 GiB),
// then a pax extension header rides along just for that entry. Docker's Go tar
// reader consumes both. The writers stream: entry bodies are read from the
// host in blocks and pushed to a sink as the archive is produced, so nothing
// buffers the whole payload (the build_* wrappers collect into a string for
// callers that want the small-payload convenience).
//
// Kept separate from DockerClient (and free of any Boost/HTTP dependency) so it
// can be unit-tested without a daemon: stream a tar, read it back with
// libarchive and assert the entry fields.
namespace testcontainers::docker {

/// Receives successive blocks of a tar stream as it is produced (up to ~64 KiB
/// each). May throw: the archive production stops and the exception propagates
/// out of the `stream_*` call unchanged (the HTTP layer uses that to abort on
/// a dead connection with its own typed error).
using TarSink = std::function<void(const char* data, std::size_t size)>;

/// Strip a single leading '/' from a path (tar entry names are relative).
/// A path without a leading slash is returned unchanged.
std::string strip_leading_slash(const std::string& path);

/// Normalize a container-side copy target into a tar entry name. A Windows
/// drive-rooted target ("C:\dir\file" or "C:/dir/file") drops the drive prefix
/// and turns '\' separators into '/' — on a Windows engine, `path=/` extraction
/// is relative to `C:\`, so the result lands where the caller pointed. Then the
/// leading '/' is stripped. Backslashes in non-drive-rooted targets are left
/// alone (a valid character in Linux file names).
std::string normalize_entry_name(const std::string& target);

/// Stream a tar archive for `source` into `sink`.
///
/// For a host-file or in-memory-bytes source the archive holds ONE regular-file
/// entry: the entry name is `source.target()` normalized (see
/// normalize_entry_name), the size and mode are set, and the body is the host
/// file's bytes (when `source.is_file()`) or `source.bytes()`. A host file is
/// read in blocks at stream time, never loaded whole.
///
/// For a directory source (`source.is_dir()`) the archive holds a directory
/// entry for the normalized target and each of its path components (so nothing
/// needs to pre-exist in the container), plus one entry per directory and
/// regular file under `source.host_path()`, in sorted order. Files carry
/// `source.mode()`; directories are 0755. Directory symlinks are not descended
/// into; non-regular files (sockets, FIFOs, dangling symlinks) are skipped.
///
/// Throws DockerError if a host file or directory cannot be read, changes size
/// mid-stream, or the archive cannot be produced; an exception thrown by
/// `sink` propagates unchanged.
void stream_tar(const CopyToContainer& source, const TarSink& sink);

/// Stream ONE tar archive holding the entries of all `sources` in order (each
/// source contributes exactly what its single-source overload would). Backs
/// the batched copy-to-container call: one archive, one PUT, N sources.
void stream_tar(const std::vector<CopyToContainer>& sources, const TarSink& sink);

/// stream_tar collected into a string — the raw tar bytes, for small payloads
/// and tests.
std::string build_tar(const CopyToContainer& source);

/// One file to place into a build-context tar.
struct TarFile {
    std::string name; ///< path within the context (e.g. "Dockerfile", "app/x.txt")
    std::string body; ///< inline file contents (may be binary); ignored when `path` is set
    int mode = 0644;  ///< octal regular-file mode
    /// When set, the entry's bytes come from this host file, read in blocks at
    /// stream time instead of being preloaded (the descriptor stays cheap no
    /// matter the file size). The file must exist and keep its size until the
    /// stream completes. Set exactly one of `body` / `path`. The "redundant"
    /// explicit default keeps 2/3-field aggregate initialization free of
    /// gcc's -Wmissing-field-initializers.
    std::filesystem::path path{}; // NOLINT(readability-redundant-member-init)
};

/// Stream a tar archive holding all `files` as regular-file entries (names used
/// verbatim, leading '/' stripped) into `sink`. Throws DockerError on failure;
/// an exception thrown by `sink` propagates unchanged. This is the
/// build-context body for `POST /build`.
void stream_context_tar(const std::vector<TarFile>& files, const TarSink& sink);

/// stream_context_tar collected into a string — the raw tar bytes.
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

/// Pull-source for a streamed tar: fill `data` (capacity `size`), return the
/// byte count provided, 0 at end of stream. May throw — the extraction stops
/// and the exception propagates unchanged (the HTTP layer signals a dead
/// download this way).
using TarSource = std::function<std::size_t(char* data, std::size_t size)>;

/// Stream-extract a tar into `dest_dir` (created if missing), reading the
/// archive from `source` block by block — the archive is never in memory
/// whole, and each file's bytes go straight to disk. Regular files and
/// directories are materialized (file permission bits applied best-effort);
/// symlinks, hardlinks, and device entries are SKIPPED. Tar-slip protection:
/// an entry whose path is absolute, drive-rooted, or contains ".." throws
/// DockerError before anything is written. Also throws on an unreadable
/// archive or a filesystem failure.
void extract_tar_to_dir(const TarSource& source, const std::filesystem::path& dest_dir);

} // namespace testcontainers::docker
