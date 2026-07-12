#include "docker/Tar.hpp"

#include "testcontainers/Error.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace testcontainers::docker {

namespace {

/// One sink call per block keeps the wire writes coarse without buffering
/// much; also the read granularity for lazy file bodies.
constexpr std::size_t kTarBlockSize = std::size_t{64} * 1024;

/// Client data for the libarchive write callback: the destination sink plus
/// the first exception it threw. libarchive is C — an exception must not
/// unwind through its frames — so the callback catches everything, stores it
/// here and reports -1 (ARCHIVE_FATAL); the C++ layer rethrows after the
/// failed libarchive call returns.
struct SinkState {
    const TarSink* sink = nullptr;
    std::exception_ptr error;
};

la_ssize_t write_to_sink(struct archive* /*a*/, void* client_data, const void* buffer,
                         std::size_t length) {
    auto* state = static_cast<SinkState*>(client_data);
    try {
        (*state->sink)(static_cast<const char*>(buffer), length);
        return static_cast<la_ssize_t>(length);
    } catch (...) {
        state->error = std::current_exception();
        return -1;
    }
}

/// Throw for a failed libarchive write call: the sink's own exception when it
/// threw one (full fidelity — a transport error keeps its type and message),
/// DockerError with libarchive's diagnostic otherwise.
[[noreturn]] void throw_write_error(const SinkState& state, struct archive* a,
                                    const std::string& ctx, const char* op) {
    if (state.error) {
        std::rethrow_exception(state.error);
    }
    const char* err = archive_error_string(a);
    throw DockerError(ctx + ": " + std::string(op) + " failed: " + (err ? err : "unknown error"));
}

/// One entry queued for a tar archive (a regular file or a directory).
struct PendingEntry {
    std::string name;             ///< relative entry pathname ('/'-separated; dirs end in '/')
    std::string body;             ///< inline file contents (used when `file` is empty)
    std::filesystem::path file;   ///< host file whose bytes are streamed at write time
    std::uintmax_t file_size = 0; ///< header size for a `file` entry (stat-time)
    int mode = 0644;              ///< permission bits
    bool is_dir = false;
};

using ArchivePtr = std::unique_ptr<struct archive, int (*)(struct archive*)>;
using EntryPtr = std::unique_ptr<struct archive_entry, void (*)(struct archive_entry*)>;

/// Stat `path` for a lazy file entry's header size. Throws DockerError
/// (prefixed by `ctx`) when the file cannot be read.
std::uintmax_t stat_size_or_throw(const std::filesystem::path& path, const std::string& ctx) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw DockerError(ctx + ": cannot open host file '" + path.string() + "': " + ec.message());
    }
    return size;
}

void write_data_or_throw(struct archive* a, const SinkState& state, const std::string& ctx,
                         const char* data, std::size_t size) {
    const la_ssize_t written = archive_write_data(a, data, size);
    if (written < 0 || static_cast<std::size_t>(written) != size) {
        throw_write_error(state, a, ctx, "archive_write_data");
    }
}

/// Write `entries` as a pax(restricted) tar, pushing the produced blocks to
/// `sink`. A lazy `file` entry's body is read in kTarBlockSize blocks here —
/// the whole payload is never in memory. `ctx` prefixes error messages (e.g.
/// "copy_to_container"). Throws DockerError on any libarchive/host-read
/// failure; an exception thrown by `sink` propagates unchanged.
void write_tar_stream(const std::vector<PendingEntry>& entries, const std::string& ctx,
                      const TarSink& sink) {
    // Declared BEFORE the archive handle on purpose: when an error below
    // unwinds the stack, archive_write_free flushes buffered data through
    // write_to_sink one last time — the callback state must still be alive.
    SinkState state;
    state.sink = &sink;

    ArchivePtr a(archive_write_new(), archive_write_free);
    if (a == nullptr) {
        throw DockerError(ctx + ": archive_write_new failed");
    }
    // pax "restricted": entries stay plain USTAR until a field does not fit
    // (a >100-char path, a >8 GiB file), then a pax extension header rides
    // along for that entry only. Docker's Go tar reader consumes both.
    archive_write_set_format_pax_restricted(a.get());
    archive_write_set_bytes_per_block(a.get(), static_cast<int>(kTarBlockSize));
    // Do not pad the final block up to bytes_per_block (tar output is already
    // 512-aligned; a full 64 KiB tail would just be zeros on the wire).
    archive_write_set_bytes_in_last_block(a.get(), 1);

    if (archive_write_open(a.get(), &state, /*open*/ nullptr, write_to_sink, /*close*/ nullptr) !=
        ARCHIVE_OK) {
        throw_write_error(state, a.get(), ctx, "archive_write_open");
    }

    std::vector<char> buf(kTarBlockSize);
    for (const PendingEntry& e : entries) {
        const bool from_file = !e.file.empty();
        EntryPtr entry(archive_entry_new(), archive_entry_free);
        if (entry == nullptr) {
            throw DockerError(ctx + ": archive_entry_new failed");
        }
        archive_entry_set_pathname(entry.get(), e.name.c_str());
        archive_entry_set_filetype(entry.get(), e.is_dir ? AE_IFDIR : AE_IFREG);
        // libarchive's mode parameter is 16-bit on Windows (CRT _mode_t);
        // permission bits always fit.
        archive_entry_set_perm(entry.get(), static_cast<unsigned short>(e.mode));
        archive_entry_set_size(entry.get(), e.is_dir    ? 0
                                            : from_file ? static_cast<la_int64_t>(e.file_size)
                                                        : static_cast<la_int64_t>(e.body.size()));

        // ARCHIVE_WARN still writes a usable header (e.g. a pathname the
        // process locale cannot convert falls back to binary hdrcharset) —
        // the read paths already tolerate WARN, so the write path must not
        // abort the whole copy over one.
        if (archive_write_header(a.get(), entry.get()) < ARCHIVE_WARN) {
            throw_write_error(state, a.get(), ctx, "archive_write_header");
        }

        if (e.is_dir) {
            continue;
        }
        if (from_file) {
            std::ifstream in(e.file, std::ios::binary);
            if (!in) {
                throw DockerError(ctx + ": cannot open host file '" + e.file.string() + "'");
            }
            std::uintmax_t remaining = e.file_size;
            while (remaining > 0) {
                const std::size_t want =
                    static_cast<std::size_t>(std::min<std::uintmax_t>(buf.size(), remaining));
                in.read(buf.data(), static_cast<std::streamsize>(want));
                const std::streamsize got = in.gcount();
                if (got <= 0) {
                    break;
                }
                write_data_or_throw(a.get(), state, ctx, buf.data(), static_cast<std::size_t>(got));
                remaining -= static_cast<std::uintmax_t>(got);
            }
            // The header committed the stat-time size: a file that shrank,
            // grew, or became unreadable mid-stream would corrupt the archive
            // framing — fail loudly (GNU tar's "file changed as we read it").
            in.peek(); // reaches EOF only if the file still ends at file_size
            if (remaining != 0 || !in.eof()) {
                throw DockerError(ctx + ": host file '" + e.file.string() +
                                  "' changed or became unreadable while being archived");
            }
        } else if (!e.body.empty()) {
            write_data_or_throw(a.get(), state, ctx, e.body.data(), e.body.size());
        }
    }

    // archive_write_close flushes the pax/ustar trailer through the sink; close
    // explicitly to surface a flush failure (free alone would swallow it).
    if (archive_write_close(a.get()) != ARCHIVE_OK) {
        throw_write_error(state, a.get(), ctx, "archive_write_close");
    }
}

/// Recursively list `dir` as tar entries rooted at `base` (the normalized
/// container target, e.g. "opt/data"): directory entries for `base` and each
/// of its path components first, then one entry per directory/regular file in
/// the tree, sorted by name (parents always precede children). Files carry
/// `file_mode` and are recorded lazily (path + stat size; bytes are read at
/// stream time); directories are 0755. Throws DockerError when `dir` is not a
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
            PendingEntry e;
            e.name = component + "/";
            e.mode = 0755;
            e.is_dir = true;
            out.push_back(std::move(e));
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
                PendingEntry e;
                e.name = name + "/";
                e.mode = 0755;
                e.is_dir = true;
                out.push_back(std::move(e));
            } else if (item.is_regular_file()) {
                PendingEntry e;
                e.name = name;
                e.file = item.path();
                e.file_size = item.file_size(); // throws into the catch below
                e.mode = file_mode;
                out.push_back(std::move(e));
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

/// The entries `source` contributes to an archive (shared by both stream_tar
/// overloads): one regular-file entry for a file/bytes source (a host file is
/// recorded lazily — path + stat size), the directory chain plus the walked
/// tree for a directory source.
std::vector<PendingEntry> collect_entries(const CopyToContainer& source) {
    const std::string name = normalize_entry_name(source.target());
    if (source.is_dir()) {
        return walk_host_dir(source.host_path(), name, source.mode());
    }

    PendingEntry e;
    e.name = name;
    e.mode = source.mode();
    if (source.is_file()) {
        e.file_size = stat_size_or_throw(source.host_path(), "copy_to_container");
        e.file = source.host_path();
    } else {
        e.body = source.bytes();
    }
    std::vector<PendingEntry> out;
    out.push_back(std::move(e));
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

void stream_tar(const CopyToContainer& source, const TarSink& sink) {
    write_tar_stream(collect_entries(source), "copy_to_container", sink);
}

void stream_tar(const std::vector<CopyToContainer>& sources, const TarSink& sink) {
    std::vector<PendingEntry> entries;
    std::unordered_set<std::string> seen_dirs;
    for (const CopyToContainer& source : sources) {
        for (PendingEntry& e : collect_entries(source)) {
            // Directory sources sharing a target prefix would each emit the
            // full chain ("opt/" twice); one directory entry is enough.
            // First-wins by name is safe because every directory entry is
            // identical today (0755, empty body) — revisit the key if
            // directory metadata ever becomes configurable. File entries are
            // NOT deduplicated: a later source legitimately overwrites an
            // earlier target (extraction is last-wins).
            if (e.is_dir && !seen_dirs.insert(e.name).second) {
                continue;
            }
            entries.push_back(std::move(e));
        }
    }
    write_tar_stream(entries, "copy_to_container", sink);
}

std::string build_tar(const CopyToContainer& source) {
    std::string out;
    stream_tar(source, [&out](const char* data, std::size_t size) { out.append(data, size); });
    return out;
}

void stream_context_tar(const std::vector<TarFile>& files, const TarSink& sink) {
    std::vector<PendingEntry> entries;
    entries.reserve(files.size());
    for (const TarFile& file : files) {
        PendingEntry e;
        e.name = strip_leading_slash(file.name);
        e.mode = file.mode;
        if (!file.path.empty()) {
            e.file_size = stat_size_or_throw(file.path, "build_context_tar");
            e.file = file.path;
        } else {
            e.body = file.body;
        }
        entries.push_back(std::move(e));
    }
    write_tar_stream(entries, "build_context_tar", sink);
}

std::string build_context_tar(const std::vector<TarFile>& files) {
    std::string out;
    stream_context_tar(files,
                       [&out](const char* data, std::size_t size) { out.append(data, size); });
    return out;
}

namespace {

/// Client data for the libarchive read callback: the pull source, its block
/// buffer, and the first exception it threw (same C-frame firewall as the
/// write side: stored here, reported as ARCHIVE_FATAL, rethrown from C++).
struct SourceState {
    const TarSource* source = nullptr;
    std::vector<char> buf;
    std::exception_ptr error;
};

la_ssize_t read_from_source(struct archive* /*a*/, void* client_data, const void** buffer) {
    auto* state = static_cast<SourceState*>(client_data);
    try {
        const std::size_t n = (*state->source)(state->buf.data(), state->buf.size());
        *buffer = state->buf.data();
        return static_cast<la_ssize_t>(n); // 0 = end of stream
    } catch (...) {
        state->error = std::current_exception();
        return -1;
    }
}

/// Throw for a failed libarchive read call: the source's own exception when
/// it threw one, DockerError with libarchive's diagnostic otherwise.
[[noreturn]] void throw_read_error(const SourceState& state, struct archive* a, const char* op) {
    if (state.error) {
        std::rethrow_exception(state.error);
    }
    const char* err = archive_error_string(a);
    throw DockerError(std::string("extract_tar_to_dir: ") + op +
                      " failed: " + (err ? err : "unknown error"));
}

/// Validate an archive entry pathname and resolve it under `dest_dir`.
/// Rejects absolute / drive-rooted names and any ".." component (tar-slip);
/// "." components are dropped. Returns nullopt for an effectively-empty name.
std::optional<std::filesystem::path> safe_dest_path(const std::string& name,
                                                    const std::filesystem::path& dest_dir) {
    if (name.empty()) {
        return std::nullopt;
    }
    const bool drive_rooted = name.size() >= 2 &&
                              std::isalpha(static_cast<unsigned char>(name[0])) != 0 &&
                              name[1] == ':';
    if (name.front() == '/' || name.front() == '\\' || drive_rooted) {
        throw DockerError("extract_tar_to_dir: refusing absolute entry path '" + name + "'");
    }
    std::filesystem::path out = dest_dir;
    bool any = false;
    std::string_view rest(name);
    while (!rest.empty()) {
        const std::size_t sep = rest.find_first_of("/\\");
        const std::string_view part = rest.substr(0, sep);
        if (part == "..") {
            throw DockerError("extract_tar_to_dir: refusing traversal entry path '" + name + "'");
        }
        if (!part.empty() && part != ".") {
            out /= part;
            any = true;
        }
        if (sep == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(sep + 1);
    }
    if (!any) {
        return std::nullopt;
    }
    // Belt and braces: on Windows a COMPONENT carrying a root-name ("C:",
    // or "a:b" — an NTFS alternate data stream) makes operator/= RESET the
    // accumulator onto another drive, silently escaping dest_dir. Verify the
    // resolved path still sits under dest_dir by this platform's own rules
    // (on POSIX a colon is an ordinary filename character and stays inside).
    const std::filesystem::path rel = out.lexically_relative(dest_dir);
    if (rel.empty() || *rel.begin() == "..") {
        throw DockerError("extract_tar_to_dir: refusing entry path escaping the destination: '" +
                          name + "'");
    }
    return out;
}

} // namespace

void extract_tar_to_dir(const TarSource& source, const std::filesystem::path& dest_dir) {
    // Declared BEFORE the archive handle: archive_read_free may still call
    // read_from_source while unwinding (mirrors the write side).
    SourceState state;
    state.source = &source;
    state.buf.resize(kTarBlockSize);

    ArchivePtr a(archive_read_new(), archive_read_free);
    if (a == nullptr) {
        throw DockerError("extract_tar_to_dir: archive_read_new failed");
    }
    archive_read_support_format_tar(a.get());

    if (archive_read_open(a.get(), &state, /*open*/ nullptr, read_from_source,
                          /*close*/ nullptr) != ARCHIVE_OK) {
        throw_read_error(state, a.get(), "archive_read_open");
    }

    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) {
        throw DockerError("extract_tar_to_dir: cannot create destination directory '" +
                          dest_dir.string() + "': " + ec.message());
    }

    struct archive_entry* entry = nullptr;
    std::vector<char> block(kTarBlockSize);
    for (;;) {
        const int rc = archive_read_next_header(a.get(), &entry);
        if (rc == ARCHIVE_EOF) {
            break;
        }
        // ARCHIVE_WARN is non-fatal (e.g. an unset field); anything below it is.
        if (rc < ARCHIVE_WARN) {
            throw_read_error(state, a.get(), "archive_read_next_header");
        }

        const char* raw_name = archive_entry_pathname(entry);
        const std::optional<std::filesystem::path> dest =
            safe_dest_path(raw_name ? raw_name : "", dest_dir);
        const auto type = archive_entry_filetype(entry);
        if (!dest || (type != AE_IFREG && type != AE_IFDIR)) {
            // Symlinks, hardlinks, devices, FIFOs: skipped by policy (a
            // symlink written to disk could redirect a later entry outside
            // dest_dir; callers needing full fidelity use the raw sink
            // overload and their own extraction).
            archive_read_data_skip(a.get());
            continue;
        }

        if (type == AE_IFDIR) {
            std::filesystem::create_directories(*dest, ec);
            if (ec) {
                throw DockerError("extract_tar_to_dir: cannot create directory '" + dest->string() +
                                  "': " + ec.message());
            }
            continue;
        }

        std::filesystem::create_directories(dest->parent_path(), ec); // best-effort; open reports
        {
            std::ofstream out(*dest, std::ios::binary | std::ios::trunc);
            if (!out) {
                throw DockerError("extract_tar_to_dir: cannot create file '" + dest->string() +
                                  "'");
            }
            for (;;) {
                const la_ssize_t n = archive_read_data(a.get(), block.data(), block.size());
                if (n == 0) {
                    break;
                }
                if (n < 0) {
                    throw_read_error(state, a.get(), "archive_read_data");
                }
                out.write(block.data(), static_cast<std::streamsize>(n));
                if (!out) {
                    throw DockerError("extract_tar_to_dir: failed writing '" + dest->string() +
                                      "'");
                }
            }
        }
        // Permission bits, best-effort (on Windows this maps to the readonly
        // attribute at most; failures are ignored on purpose).
        std::filesystem::permissions(
            *dest, static_cast<std::filesystem::perms>(archive_entry_perm(entry) & 0777),
            std::filesystem::perm_options::replace, ec);
    }
}

std::vector<TarEntry> extract_tar(const std::string& tar_bytes) {
    ArchivePtr a(archive_read_new(), archive_read_free);
    if (a == nullptr) {
        throw DockerError("extract_tar: archive_read_new failed");
    }
    archive_read_support_format_tar(a.get());

    // The ArchivePtr frees the handle on every exit; throwing needs no manual
    // cleanup (extract_tar_to_dir works the same way).
    const auto fail = [&a](const char* what) {
        const char* err = archive_error_string(a.get());
        throw DockerError("extract_tar: " + std::string(what) +
                          " failed: " + (err != nullptr ? err : "unknown error"));
    };

    if (archive_read_open_memory(a.get(), tar_bytes.data(), tar_bytes.size()) != ARCHIVE_OK) {
        fail("archive_read_open_memory");
    }

    std::vector<TarEntry> entries;
    struct archive_entry* entry = nullptr;
    for (;;) {
        const int rc = archive_read_next_header(a.get(), &entry);
        if (rc == ARCHIVE_EOF) {
            break;
        }
        // ARCHIVE_WARN is non-fatal (e.g. an unset field); anything below it is.
        if (rc < ARCHIVE_WARN) {
            fail("archive_read_next_header");
        }

        TarEntry out;
        const char* name = archive_entry_pathname(entry);
        out.name = name ? name : "";
        out.mode = static_cast<int>(archive_entry_perm(entry));
        out.is_regular_file = (archive_entry_filetype(entry) == AE_IFREG);

        if (out.is_regular_file) {
            char buf[4096];
            la_ssize_t n = 0;
            while ((n = archive_read_data(a.get(), buf, sizeof(buf))) > 0) {
                out.body.append(buf, static_cast<std::size_t>(n));
            }
            if (n < 0) {
                fail("archive_read_data");
            }
        }

        entries.push_back(std::move(out));
    }

    return entries;
}

} // namespace testcontainers::docker
