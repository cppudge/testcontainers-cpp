#include "docker/Tar.hpp"

#include "FileRead.hpp"
#include "testcontainers/Error.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace testcontainers::docker {

namespace {

// libarchive write callback: append the bytes it hands us to a std::string. The
// client_data is the destination buffer. Returning the byte count signals all
// bytes were consumed; returning -1 would signal an error.
la_ssize_t append_to_string(struct archive* /*a*/, void* client_data, const void* buffer,
                            std::size_t length) {
    auto* out = static_cast<std::string*>(client_data);
    out->append(static_cast<const char*>(buffer), length);
    return static_cast<la_ssize_t>(length);
}

/// One entry queued for a tar archive (a regular file or a directory).
struct PendingEntry {
    std::string name; ///< relative entry pathname ('/'-separated; dirs end in '/')
    std::string body; ///< file contents (empty for directories)
    int mode = 0644;  ///< permission bits
    bool is_dir = false;
};

/// Write `entries` into a USTAR archive and return the raw tar bytes. `ctx`
/// prefixes error messages (e.g. "copy_to_container"). Throws DockerError on
/// any libarchive failure.
std::string write_tar(const std::vector<PendingEntry>& entries, const std::string& ctx) {
    struct archive* a = archive_write_new();
    if (a == nullptr) {
        throw DockerError(ctx + ": archive_write_new failed");
    }
    // USTAR is the simplest portable format Docker accepts.
    archive_write_set_format_ustar(a);

    std::string out;
    if (archive_write_open(a, &out, /*open*/ nullptr, append_to_string, /*close*/ nullptr) !=
        ARCHIVE_OK) {
        const std::string err = archive_error_string(a) ? archive_error_string(a) : "unknown error";
        archive_write_free(a);
        throw DockerError(ctx + ": archive_write_open failed: " + err);
    }

    for (const PendingEntry& e : entries) {
        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, e.name.c_str());
        archive_entry_set_filetype(entry, e.is_dir ? AE_IFDIR : AE_IFREG);
        // libarchive's mode parameter is 16-bit on Windows (CRT _mode_t);
        // permission bits always fit.
        archive_entry_set_perm(entry, static_cast<unsigned short>(e.mode));
        archive_entry_set_size(entry, e.is_dir ? 0 : static_cast<la_int64_t>(e.body.size()));

        if (archive_write_header(a, entry) != ARCHIVE_OK) {
            const std::string err =
                archive_error_string(a) ? archive_error_string(a) : "unknown error";
            archive_entry_free(entry);
            archive_write_free(a);
            throw DockerError(ctx + ": archive_write_header failed: " + err);
        }
        if (!e.is_dir && !e.body.empty()) {
            const la_ssize_t written = archive_write_data(a, e.body.data(), e.body.size());
            if (written < 0 || static_cast<std::size_t>(written) != e.body.size()) {
                const std::string err =
                    archive_error_string(a) ? archive_error_string(a) : "short write";
                archive_entry_free(entry);
                archive_write_free(a);
                throw DockerError(ctx + ": archive_write_data failed: " + err);
            }
        }
        archive_entry_free(entry);
    }

    // archive_write_close flushes the trailing blocks; archive_write_free closes
    // implicitly but we close explicitly to surface any flush error.
    if (archive_write_close(a) != ARCHIVE_OK) {
        const std::string err = archive_error_string(a) ? archive_error_string(a) : "unknown error";
        archive_write_free(a);
        throw DockerError(ctx + ": archive_write_close failed: " + err);
    }
    archive_write_free(a);
    return out;
}

/// Recursively list `dir` as tar entries rooted at `base` (the normalized
/// container target, e.g. "opt/data"): directory entries for `base` and each
/// of its path components first, then one entry per directory/regular file in
/// the tree, sorted by name (parents always precede children). Files carry
/// `file_mode`; directories are 0755. Throws DockerError when `dir` is not a
/// directory or the walk fails.
std::vector<PendingEntry> walk_host_dir(const std::filesystem::path& dir, const std::string& base,
                                        int file_mode) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        throw DockerError("copy_to_container: host directory '" + dir.string() +
                          "' does not exist or is not a directory");
    }

    // A trailing '/' on the target ("/opt/data/") would double the separator in
    // every entry name; drop it so both spellings produce the same archive.
    std::string root = base;
    if (!root.empty() && root.back() == '/') {
        root.pop_back();
    }

    std::vector<PendingEntry> out;
    // Directory entries for the target and every intermediate component, so the
    // copy does not depend on the target chain pre-existing in the image
    // (extraction treats an already-existing directory as a no-op).
    for (std::size_t pos = root.find('/');; pos = root.find('/', pos + 1)) {
        const std::string component = (pos == std::string::npos) ? root : root.substr(0, pos);
        if (!component.empty()) {
            out.push_back(PendingEntry{component + "/", "", 0755, true});
        }
        if (pos == std::string::npos) {
            break;
        }
    }

    try {
        for (const std::filesystem::directory_entry& item :
             std::filesystem::recursive_directory_iterator(dir)) {
            // u8string keeps entry names UTF-8 regardless of the host code page
            // (generic_string would re-encode through the ANSI page on Windows).
            const std::u8string rel8 = item.path().lexically_relative(dir).generic_u8string();
            const std::string rel(rel8.begin(), rel8.end());
            const std::string name = root.empty() ? rel : root + "/" + rel;
            if (item.is_directory()) {
                // Not descended into when it is a symlink (iterator default);
                // it still lands as a (possibly empty) directory.
                out.push_back(PendingEntry{name + "/", "", 0755, true});
            } else if (item.is_regular_file()) {
                out.push_back(
                    PendingEntry{name, detail::read_file_or_throw(item.path(), "copy_to_container"),
                                 file_mode, false});
            }
            // Anything else (sockets, FIFOs, dangling symlinks) is skipped.
        }
    } catch (const std::filesystem::filesystem_error& e) {
        throw DockerError("copy_to_container: cannot walk host directory '" + dir.string() +
                          "': " + e.what());
    }

    // Directory iteration order is unspecified; sort for a deterministic
    // archive. Lexicographic order keeps parents before their children.
    std::sort(out.begin(), out.end(),
              [](const PendingEntry& a, const PendingEntry& b) { return a.name < b.name; });
    return out;
}

} // namespace

std::string strip_leading_slash(const std::string& path) {
    if (!path.empty() && path.front() == '/') {
        return path.substr(1);
    }
    return path;
}

std::string normalize_entry_name(const std::string& target) {
    std::string t = target;
    // "C:\..." / "C:/..." — a drive-rooted Windows container path: the drive is
    // implied by the extraction root, and tar entry separators are '/'.
    if (t.size() >= 3 && std::isalpha(static_cast<unsigned char>(t[0])) != 0 && t[1] == ':' &&
        (t[2] == '\\' || t[2] == '/')) {
        t.erase(0, 2);
        std::replace(t.begin(), t.end(), '\\', '/');
    }
    return strip_leading_slash(t);
}

std::string build_tar(const CopyToContainer& source) {
    const std::string name = normalize_entry_name(source.target());

    std::vector<PendingEntry> entries;
    if (source.is_dir()) {
        entries = walk_host_dir(source.host_path(), name, source.mode());
    } else {
        // The entry body: either the host file's bytes or the in-memory bytes.
        std::string body = source.is_file()
                               ? detail::read_file_or_throw(source.host_path(), "copy_to_container")
                               : source.bytes();
        entries.push_back(PendingEntry{name, std::move(body), source.mode(), false});
    }
    return write_tar(entries, "copy_to_container");
}

std::string build_context_tar(const std::vector<TarFile>& files) {
    std::vector<PendingEntry> entries;
    entries.reserve(files.size());
    for (const TarFile& file : files) {
        entries.push_back(
            PendingEntry{strip_leading_slash(file.name), file.body, file.mode, false});
    }
    return write_tar(entries, "build_context_tar");
}

std::vector<TarEntry> extract_tar(const std::string& tar_bytes) {
    struct archive* a = archive_read_new();
    if (a == nullptr) {
        throw DockerError("extract_tar: archive_read_new failed");
    }
    archive_read_support_format_tar(a);

    if (archive_read_open_memory(a, tar_bytes.data(), tar_bytes.size()) != ARCHIVE_OK) {
        const std::string err = archive_error_string(a) ? archive_error_string(a) : "unknown error";
        archive_read_free(a);
        throw DockerError("extract_tar: archive_read_open_memory failed: " + err);
    }

    std::vector<TarEntry> entries;
    struct archive_entry* entry = nullptr;
    for (;;) {
        const int rc = archive_read_next_header(a, &entry);
        if (rc == ARCHIVE_EOF) {
            break;
        }
        // ARCHIVE_WARN is non-fatal (e.g. an unset field); anything below it is.
        if (rc < ARCHIVE_WARN) {
            const std::string err =
                archive_error_string(a) ? archive_error_string(a) : "unknown error";
            archive_read_free(a);
            throw DockerError("extract_tar: archive_read_next_header failed: " + err);
        }

        TarEntry out;
        const char* name = archive_entry_pathname(entry);
        out.name = name ? name : "";
        out.mode = static_cast<int>(archive_entry_perm(entry));
        out.is_regular_file = (archive_entry_filetype(entry) == AE_IFREG);

        if (out.is_regular_file) {
            char buf[4096];
            la_ssize_t n = 0;
            while ((n = archive_read_data(a, buf, sizeof(buf))) > 0) {
                out.body.append(buf, static_cast<std::size_t>(n));
            }
            if (n < 0) {
                const std::string err =
                    archive_error_string(a) ? archive_error_string(a) : "unknown error";
                archive_read_free(a);
                throw DockerError("extract_tar: archive_read_data failed: " + err);
            }
        }

        entries.push_back(std::move(out));
    }

    archive_read_free(a);
    return entries;
}

} // namespace testcontainers::docker
