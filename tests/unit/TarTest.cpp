#include <gtest/gtest.h>

#include <archive.h>
#include <archive_entry.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "docker/Tar.hpp"
#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/Error.hpp"

// Tests in this file:
//   Tar.StripLeadingSlash - strip_leading_slash removes exactly one leading slash and leaves a relative path untouched.
//   Tar.ContentEntryFields - a content source builds a tar whose single entry has the slash-stripped pathname, the byte size, the content, and the default 0644 mode.
//   Tar.HostFileRoundTrips - a host-file source reads the file and the tar entry's content matches the file's bytes.
//   Tar.ModeIsReflected - with_mode(0600) is reflected in the built entry's permission bits.
//   Tar.BinaryPayloadRoundTrips - a payload with embedded NUL bytes round-trips byte-for-byte through the tar.
//   Tar.MissingHostFileThrows - building a tar from a non-existent host file throws DockerError.
//   Tar.ExtractRoundTrip - build_tar then extract_tar yields one regular-file entry with the slash-stripped name, body, and mode preserved.
//   Tar.ExtractBinaryRoundTrip - a body with embedded NUL bytes survives build_tar then extract_tar byte-for-byte.
//   Tar.ContextTarTwoFiles - build_context_tar packs two files and extract_tar reads both back with their names, bodies, and modes.
//   Tar.NormalizeEntryName - drive-rooted Windows targets drop the drive and use '/' separators; Unix targets only lose the leading slash (backslashes in them stay put).
//   Tar.WindowsDriveTargetNormalized - build_tar with a "C:\..." target stores the drive-stripped, slash-separated entry name.
//   Tar.DirSourceEntries - a host_dir source emits the target's directory chain plus every subdirectory (0755) and file (with body), sorted with parents first; empty directories are preserved.
//   Tar.DirSourceModeAppliesToFiles - with_mode on a host_dir source applies to every file entry while directories stay 0755.
//   Tar.DirSourceWindowsTarget - a host_dir source with a "C:\..." target roots the tree under the drive-stripped name.
//   Tar.DirSourceTrailingSlashTarget - "/opt/data/" and "/opt/data" produce byte-identical archives (no doubled separators).
//   Tar.MissingHostDirThrows - a host_dir source pointing at a missing path or at a regular file throws DockerError.
//   Tar.PaxLongPathRoundTrips - a flat >100-char entry name (unrepresentable in plain USTAR) round-trips via the pax extension header.
//   Tar.StreamHostFileInBlocks - a ~200 KB host file is streamed to the sink in multiple 64 KiB blocks and the reassembled archive round-trips byte-for-byte.
//   Tar.SinkExceptionPropagates - an exception thrown by the sink at trailer-flush time surfaces from stream_tar with its type and message intact.
//   Tar.SinkExceptionPropagatesMidStream - a sink that throws mid-body (large file) also surfaces unchanged.
//   Tar.BatchedSourcesShareOneArchive - the vector stream_tar overload packs every source's entries into one archive in order.
//   Tar.BatchedSourcesDedupSharedDirChains - directory sources sharing a target prefix emit the shared chain's directory entries once (files are never deduplicated).
//   Tar.ContextTarLazyFileEntry - a TarFile with `path` set streams the host file's bytes (the inline body is ignored).
//   Tar.ContextTarMissingLazyFileThrows - a TarFile whose `path` does not exist throws DockerError.
//   Tar.ZeroByteLazyFileEntry - a zero-byte host file round-trips as an empty regular-file entry (the exact-EOF edge of the changed-size check).
//   Tar.ExtractToDirRoundTrip - extract_tar_to_dir pulls a streamed archive block by block and materializes files, nested directories, and (POSIX) permission bits under dest.
//   Tar.ExtractToDirRejectsTraversal - a "../evil" entry throws DockerError and writes nothing outside dest.
//   Tar.ExtractToDirRejectsAbsolute - an absolute "/abs" entry (hand-built archive) throws DockerError.
//   Tar.ExtractToDirDriveComponent - a "data/C:/evil" entry cannot escape onto another drive: on Windows it throws, on POSIX the colon is an ordinary character and the file stays inside dest.
//   Tar.ExtractToDirSkipsSymlinks - a symlink entry is skipped by policy while surrounding regular files extract.
//   Tar.ExtractToDirSourceExceptionPropagates - an exception thrown by the pull source surfaces unchanged (type and message).

using testcontainers::CopyToContainer;
using testcontainers::DockerError;
using testcontainers::docker::build_context_tar;
using testcontainers::docker::build_tar;
using testcontainers::docker::extract_tar;
using testcontainers::docker::extract_tar_to_dir;
using testcontainers::docker::normalize_entry_name;
using testcontainers::docker::stream_tar;
using testcontainers::docker::strip_leading_slash;
using testcontainers::docker::TarEntry;
using testcontainers::docker::TarFile;
using testcontainers::docker::TarSource;

namespace {

// One decoded tar entry, read back with libarchive.
struct ReadEntry {
    std::string pathname;
    std::string data;
    int mode = 0; // permission bits only (mask 0777)
    std::int64_t size = 0;
};

// Read the FIRST entry out of an in-memory tar. Fails the test if the archive
// can't be opened or has no entry.
ReadEntry read_first_entry(const std::string& tar) {
    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    const int open_rc = archive_read_open_memory(a, tar.data(), tar.size());
    EXPECT_EQ(open_rc, ARCHIVE_OK) << "archive_read_open_memory failed";

    ReadEntry out;
    struct archive_entry* entry = nullptr;
    const int rc = archive_read_next_header(a, &entry);
    EXPECT_EQ(rc, ARCHIVE_OK) << "archive_read_next_header found no entry";
    if (rc == ARCHIVE_OK) {
        out.pathname = archive_entry_pathname(entry);
        out.mode = static_cast<int>(archive_entry_perm(entry)) & 0777;
        out.size = static_cast<std::int64_t>(archive_entry_size(entry));

        char buf[4096];
        la_ssize_t n = 0;
        while ((n = archive_read_data(a, buf, sizeof(buf))) > 0) {
            out.data.append(buf, static_cast<std::size_t>(n));
        }
    }

    archive_read_free(a);
    return out;
}

// A self-cleaning temp directory tree for the host_dir tests:
//   root.txt ("root-body"), sub/nested.txt ("nested-body"), empty/ (no files).
class TempTree {
public:
    TempTree() {
        static std::atomic<unsigned> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("tc_tartree_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(dir_ / "sub");
        std::filesystem::create_directories(dir_ / "empty");
        std::ofstream(dir_ / "root.txt", std::ios::binary) << "root-body";
        std::ofstream(dir_ / "sub" / "nested.txt", std::ios::binary) << "nested-body";
    }
    ~TempTree() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    const std::filesystem::path& path() const { return dir_; }

private:
    std::filesystem::path dir_;
};

// A self-cleaning temp file holding `content`.
class TempFile {
public:
    explicit TempFile(const std::string& content) {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("tc_tartest_" + std::to_string(counter.fetch_add(1)));
        std::ofstream out(path_, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    std::string string() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

} // namespace

TEST(Tar, StripLeadingSlash) {
    EXPECT_EQ(strip_leading_slash("/tmp/foo.txt"), "tmp/foo.txt");
    EXPECT_EQ(strip_leading_slash("already/relative"), "already/relative");
    EXPECT_EQ(strip_leading_slash("/"), "");
    EXPECT_EQ(strip_leading_slash(""), "");
}

TEST(Tar, ContentEntryFields) {
    const std::string tar = build_tar(CopyToContainer::content("hello", "/tmp/foo.txt"));
    const ReadEntry e = read_first_entry(tar);

    EXPECT_EQ(e.pathname, "tmp/foo.txt"); // leading slash stripped
    EXPECT_EQ(e.size, 5);
    EXPECT_EQ(e.data, "hello");
    EXPECT_EQ(e.mode, 0644); // default mode
}

TEST(Tar, HostFileRoundTrips) {
    const std::string content = "from-a-host-file\nwith two lines\n";
    const TempFile file(content);

    const std::string tar = build_tar(CopyToContainer::host_file(file.string(), "/data/in.txt"));
    const ReadEntry e = read_first_entry(tar);

    EXPECT_EQ(e.pathname, "data/in.txt");
    EXPECT_EQ(e.data, content);
    EXPECT_EQ(e.size, static_cast<std::int64_t>(content.size()));
}

TEST(Tar, ModeIsReflected) {
    const std::string tar = build_tar(CopyToContainer::content("x", "/tmp/secret").with_mode(0600));
    const ReadEntry e = read_first_entry(tar);
    EXPECT_EQ(e.mode, 0600);
}

TEST(Tar, BinaryPayloadRoundTrips) {
    // Embedded NUL bytes plus high bytes — must survive byte-for-byte.
    std::string payload;
    payload.push_back('\x00');
    payload.push_back('\x01');
    payload.push_back('\xff');
    payload.push_back('\x00');
    payload.append("tail");

    const std::string tar = build_tar(CopyToContainer::content(payload, "/bin/blob"));
    const ReadEntry e = read_first_entry(tar);

    EXPECT_EQ(e.pathname, "bin/blob");
    EXPECT_EQ(e.size, static_cast<std::int64_t>(payload.size()));
    EXPECT_EQ(e.data, payload);
}

TEST(Tar, MissingHostFileThrows) {
    const std::string missing =
        (std::filesystem::temp_directory_path() / "tc_tartest_definitely_missing_file").string();
    EXPECT_THROW(build_tar(CopyToContainer::host_file(missing, "/tmp/x")), DockerError);
}

TEST(Tar, ExtractRoundTrip) {
    const std::string tar =
        build_tar(CopyToContainer::content("hello world", "/tmp/a.txt").with_mode(0644));
    const std::vector<TarEntry> entries = extract_tar(tar);

    ASSERT_EQ(entries.size(), 1u);
    const TarEntry& e = entries.front();
    EXPECT_EQ(e.name, "tmp/a.txt"); // leading slash stripped by build_tar
    EXPECT_EQ(e.body, "hello world");
    EXPECT_TRUE(e.is_regular_file);
    EXPECT_EQ(e.mode & 0777, 0644);
}

TEST(Tar, ExtractBinaryRoundTrip) {
    const std::string payload("a\0b\0c", 5); // embedded NULs
    const std::string tar = build_tar(CopyToContainer::content(payload, "/bin/blob"));
    const std::vector<TarEntry> entries = extract_tar(tar);

    ASSERT_EQ(entries.size(), 1u);
    const TarEntry& e = entries.front();
    EXPECT_EQ(e.name, "bin/blob");
    EXPECT_TRUE(e.is_regular_file);
    EXPECT_EQ(e.body, payload); // byte-exact, including the NULs
    EXPECT_EQ(e.body.size(), 5u);
}

TEST(Tar, NormalizeEntryName) {
    // Drive-rooted Windows targets: drop the drive, '/'-separate, no leading '/'.
    EXPECT_EQ(normalize_entry_name("C:\\ISS_MEDIA\\f.bin"), "ISS_MEDIA/f.bin");
    EXPECT_EQ(normalize_entry_name("c:/data/x.txt"), "data/x.txt");
    EXPECT_EQ(normalize_entry_name("C:\\"), "");

    // Unix targets: only the leading slash is stripped...
    EXPECT_EQ(normalize_entry_name("/tmp/foo.txt"), "tmp/foo.txt");
    EXPECT_EQ(normalize_entry_name("relative/name"), "relative/name");
    // ...and a backslash inside a Linux file name is a valid character, kept as-is.
    EXPECT_EQ(normalize_entry_name("/tmp/we\\ird"), "tmp/we\\ird");
}

TEST(Tar, WindowsDriveTargetNormalized) {
    const std::string tar = build_tar(CopyToContainer::content("win", "C:\\ISS_MEDIA\\f.bin"));
    const ReadEntry e = read_first_entry(tar);
    EXPECT_EQ(e.pathname, "ISS_MEDIA/f.bin");
    EXPECT_EQ(e.data, "win");
}

TEST(Tar, DirSourceEntries) {
    const TempTree tree;
    const std::string tar = build_tar(CopyToContainer::host_dir(tree.path(), "/opt/data"));
    const std::vector<TarEntry> entries = extract_tar(tar);

    // Sorted, parents first: the target chain, then the tree's dirs and files.
    ASSERT_EQ(entries.size(), 6u);
    EXPECT_EQ(entries[0].name, "opt/");
    EXPECT_EQ(entries[1].name, "opt/data/");
    EXPECT_EQ(entries[2].name, "opt/data/empty/"); // empty directory preserved
    EXPECT_EQ(entries[3].name, "opt/data/root.txt");
    EXPECT_EQ(entries[4].name, "opt/data/sub/");
    EXPECT_EQ(entries[5].name, "opt/data/sub/nested.txt");

    for (const std::size_t dir_idx :
         {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
        EXPECT_FALSE(entries[dir_idx].is_regular_file) << entries[dir_idx].name;
        EXPECT_EQ(entries[dir_idx].mode & 0777, 0755) << entries[dir_idx].name;
    }
    EXPECT_TRUE(entries[3].is_regular_file);
    EXPECT_EQ(entries[3].body, "root-body");
    EXPECT_EQ(entries[3].mode & 0777, 0644); // default file mode
    EXPECT_TRUE(entries[5].is_regular_file);
    EXPECT_EQ(entries[5].body, "nested-body");
}

TEST(Tar, DirSourceModeAppliesToFiles) {
    const TempTree tree;
    const std::string tar =
        build_tar(CopyToContainer::host_dir(tree.path(), "/opt/data").with_mode(0600));
    for (const TarEntry& e : extract_tar(tar)) {
        EXPECT_EQ(e.mode & 0777, e.is_regular_file ? 0600 : 0755) << e.name;
    }
}

TEST(Tar, DirSourceWindowsTarget) {
    const TempTree tree;
    const std::string tar = build_tar(CopyToContainer::host_dir(tree.path(), "C:\\data"));
    const std::vector<TarEntry> entries = extract_tar(tar);

    ASSERT_FALSE(entries.empty());
    EXPECT_EQ(entries[0].name, "data/"); // single-component target chain
    for (const TarEntry& e : entries) {
        EXPECT_EQ(e.name.rfind("data/", 0), 0u) << e.name; // all rooted under it
    }
}

TEST(Tar, DirSourceTrailingSlashTarget) {
    const TempTree tree;
    const std::string with_slash = build_tar(CopyToContainer::host_dir(tree.path(), "/opt/data/"));
    const std::string without = build_tar(CopyToContainer::host_dir(tree.path(), "/opt/data"));
    EXPECT_EQ(with_slash, without); // same archive byte-for-byte
}

TEST(Tar, MissingHostDirThrows) {
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "tc_tartest_definitely_missing_dir";
    EXPECT_THROW(build_tar(CopyToContainer::host_dir(missing, "/opt/x")), DockerError);

    // A regular file is not a directory source either.
    const TempFile file("not-a-dir");
    EXPECT_THROW(build_tar(CopyToContainer::host_dir(file.string(), "/opt/x")), DockerError);
}

TEST(Tar, PaxLongPathRoundTrips) {
    // A flat 154-char name cannot be represented in plain USTAR (the name
    // field caps at 100 chars and the prefix split needs a '/'); the pax
    // "restricted" writer adds an extension header for exactly this entry.
    const std::string long_name = std::string(150, 'x') + ".txt";
    const std::string tar = build_tar(CopyToContainer::content("deep", "/" + long_name));
    const std::vector<TarEntry> entries = extract_tar(tar);

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().name, long_name);
    EXPECT_EQ(entries.front().body, "deep");
    EXPECT_TRUE(entries.front().is_regular_file);
}

TEST(Tar, StreamHostFileInBlocks) {
    // ~200 KB host file: streamed to the sink in 64 KiB blocks (several
    // calls), never preloaded; the reassembled archive round-trips.
    std::string content;
    for (int i = 0; content.size() < 200'000; ++i) {
        content += "block payload line " + std::to_string(i) + "\n";
    }
    const TempFile file(content);

    std::string tar;
    std::size_t calls = 0;
    stream_tar(CopyToContainer::host_file(file.string(), "/data/big.bin"),
               [&](const char* data, std::size_t size) {
                   ++calls;
                   tar.append(data, size);
               });

    EXPECT_GE(calls, 3u); // 200 KB comes through in 64 KiB blocks
    const std::vector<TarEntry> entries = extract_tar(tar);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().name, "data/big.bin");
    EXPECT_EQ(entries.front().body, content);
}

TEST(Tar, SinkExceptionPropagates) {
    // The sink's exception must surface from stream_tar with its type and
    // message intact (the HTTP layer aborts an upload by throwing out of the
    // sink). A small archive is buffered in full, so the sink first runs at
    // trailer-flush (archive_write_close) time — the flush error path.
    const auto boom = [](const char*, std::size_t) { throw std::runtime_error("wire broke"); };
    try {
        stream_tar(CopyToContainer::content("x", "/f"), boom);
        FAIL() << "stream_tar did not propagate the sink exception";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "wire broke");
    }
}

TEST(Tar, SinkExceptionPropagatesMidStream) {
    // A >64 KiB body makes the sink run during archive_write_data — the
    // mid-body error path, distinct from the trailer flush above.
    const std::string content(200'000, 'q');
    const TempFile file(content);
    std::size_t calls = 0;
    const auto boom = [&calls](const char*, std::size_t) {
        if (++calls == 2) {
            throw std::runtime_error("wire broke mid-stream");
        }
    };
    try {
        stream_tar(CopyToContainer::host_file(file.string(), "/data/big.bin"), boom);
        FAIL() << "stream_tar did not propagate the sink exception";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "wire broke mid-stream");
    }
}

TEST(Tar, BatchedSourcesShareOneArchive) {
    const TempFile file("file-body");
    const std::vector<CopyToContainer> sources = {
        CopyToContainer::content("first", "/a/one.txt"),
        CopyToContainer::host_file(file.string(), "/b/two.txt").with_mode(0600),
    };

    std::string tar;
    stream_tar(sources, [&tar](const char* data, std::size_t size) { tar.append(data, size); });
    const std::vector<TarEntry> entries = extract_tar(tar);

    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "a/one.txt");
    EXPECT_EQ(entries[0].body, "first");
    EXPECT_EQ(entries[1].name, "b/two.txt");
    EXPECT_EQ(entries[1].body, "file-body");
    EXPECT_EQ(entries[1].mode & 0777, 0600);
}

TEST(Tar, BatchedSourcesDedupSharedDirChains) {
    const TempTree tree_a;
    const TempTree tree_b;
    const std::vector<CopyToContainer> sources = {
        CopyToContainer::host_dir(tree_a.path(), "/opt/a"),
        CopyToContainer::host_dir(tree_b.path(), "/opt/b"),
    };

    std::string tar;
    stream_tar(sources, [&tar](const char* data, std::size_t size) { tar.append(data, size); });

    // The shared "opt/" chain appears exactly once; each source keeps its own
    // subtree entries.
    std::size_t opt_count = 0;
    std::size_t a_root = 0;
    std::size_t b_root = 0;
    for (const TarEntry& e : extract_tar(tar)) {
        opt_count += (e.name == "opt/") ? 1 : 0;
        a_root += (e.name == "opt/a/") ? 1 : 0;
        b_root += (e.name == "opt/b/") ? 1 : 0;
    }
    EXPECT_EQ(opt_count, 1u);
    EXPECT_EQ(a_root, 1u);
    EXPECT_EQ(b_root, 1u);
}

TEST(Tar, ContextTarLazyFileEntry) {
    const TempFile file("lazy-context-bytes");
    TarFile lazy;
    lazy.name = "app/data.bin";
    lazy.body = "ignored when path is set";
    lazy.path = std::filesystem::path(file.string());

    const std::string tar = build_context_tar({lazy, TarFile{"Dockerfile", "FROM scratch\n"}});
    const std::vector<TarEntry> entries = extract_tar(tar);

    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "app/data.bin");
    EXPECT_EQ(entries[0].body, "lazy-context-bytes"); // the host file's bytes, not `body`
    EXPECT_EQ(entries[1].name, "Dockerfile");
    EXPECT_EQ(entries[1].body, "FROM scratch\n");
}

TEST(Tar, ContextTarMissingLazyFileThrows) {
    TarFile lazy;
    lazy.name = "x";
    lazy.path = std::filesystem::temp_directory_path() / "tc_tartest_missing_ctx_file";
    EXPECT_THROW(build_context_tar({lazy}), DockerError);
}

TEST(Tar, ZeroByteLazyFileEntry) {
    const TempFile file("");
    const std::string tar = build_tar(CopyToContainer::host_file(file.string(), "/tmp/empty"));
    const std::vector<TarEntry> entries = extract_tar(tar);

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().name, "tmp/empty");
    EXPECT_TRUE(entries.front().is_regular_file);
    EXPECT_EQ(entries.front().body, "");
}

namespace {

/// A pull source reading sequentially from an in-memory tar string.
TarSource source_from(const std::string& tar, std::size_t* pos) {
    return [&tar, pos](char* data, std::size_t size) -> std::size_t {
        const std::size_t n = tar.copy(data, size, *pos); // clamps at the tail
        *pos += n;
        return n;
    };
}

/// A self-cleaning destination directory for extract_tar_to_dir tests.
class DestDir {
public:
    DestDir() {
        static std::atomic<unsigned> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("tc_tarextract_" + std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec); // a previous crashed run's leftovers
    }
    ~DestDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    const std::filesystem::path& path() const { return dir_; }

private:
    std::filesystem::path dir_;
};

/// Read `path` fully (binary).
std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Hand-build a tar whose single entry keeps `name` VERBATIM (our writers
/// normalize names, so hostile shapes need a raw writer). `symlink_target`
/// non-empty makes it a symlink entry instead of a regular file.
std::string raw_tar_entry(const std::string& name, const std::string& body,
                          const std::string& symlink_target = {}) {
    std::string out;
    struct archive* a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    archive_write_open(
        a, &out, nullptr,
        [](struct archive*, void* dest, const void* buf, std::size_t len) -> la_ssize_t {
            static_cast<std::string*>(dest)->append(static_cast<const char*>(buf), len);
            return static_cast<la_ssize_t>(len);
        },
        nullptr);
    struct archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname(entry, name.c_str());
    if (!symlink_target.empty()) {
        archive_entry_set_filetype(entry, AE_IFLNK);
        archive_entry_set_symlink(entry, symlink_target.c_str());
        archive_entry_set_size(entry, 0);
    } else {
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_size(entry, static_cast<la_int64_t>(body.size()));
    }
    archive_entry_set_perm(entry, 0644);
    archive_write_header(a, entry);
    if (symlink_target.empty() && !body.empty()) {
        archive_write_data(a, body.data(), body.size());
    }
    archive_entry_free(entry);
    archive_write_close(a);
    archive_write_free(a);
    return out;
}

} // namespace

TEST(Tar, ExtractToDirRoundTrip) {
    const TempTree tree;
    std::string tar;
    stream_tar(CopyToContainer::host_dir(tree.path(), "/data").with_mode(0640),
               [&tar](const char* data, std::size_t size) { tar.append(data, size); });

    const DestDir dest;
    std::size_t pos = 0;
    extract_tar_to_dir(source_from(tar, &pos), dest.path());

    EXPECT_EQ(slurp(dest.path() / "data" / "root.txt"), "root-body");
    EXPECT_EQ(slurp(dest.path() / "data" / "sub" / "nested.txt"), "nested-body");
    EXPECT_TRUE(std::filesystem::is_directory(dest.path() / "data" / "empty"));
#if !defined(_WIN32)
    // Permission bits are applied on POSIX (best-effort elsewhere).
    const auto perms = std::filesystem::status(dest.path() / "data" / "root.txt").permissions();
    EXPECT_EQ(static_cast<unsigned>(perms) & 0777u, 0640u);
#endif
}

TEST(Tar, ExtractToDirRejectsTraversal) {
    const std::string tar = build_context_tar({TarFile{"../evil.txt", "boom"}});
    const DestDir dest;
    std::size_t pos = 0;
    EXPECT_THROW(extract_tar_to_dir(source_from(tar, &pos), dest.path()), DockerError);
    EXPECT_FALSE(std::filesystem::exists(dest.path().parent_path() / "evil.txt"));
}

TEST(Tar, ExtractToDirRejectsAbsolute) {
    const std::string tar = raw_tar_entry("/abs.txt", "boom");
    const DestDir dest;
    std::size_t pos = 0;
    EXPECT_THROW(extract_tar_to_dir(source_from(tar, &pos), dest.path()), DockerError);
}

TEST(Tar, ExtractToDirDriveComponent) {
    // An interior drive-letter component: std::filesystem's operator/= RESETS
    // onto that drive on Windows when the letter differs from dest's,
    // escaping dest — the extraction must refuse it there. On POSIX a colon
    // is an ordinary filename character and the entry lands inside dest.
    const DestDir dest;
#if defined(_WIN32)
    // A drive guaranteed to differ from dest's (same-letter "C:" components
    // append harmlessly; the reset needs a mismatch).
    const std::string other =
        (dest.path().root_name() == std::filesystem::path("Z:")) ? "Y:" : "Z:";
    {
        const std::string tar = raw_tar_entry("data/" + other + "/evil.txt", "boom");
        std::size_t pos = 0;
        EXPECT_THROW(extract_tar_to_dir(source_from(tar, &pos), dest.path()), DockerError);
    }
    {
        // An NTFS alternate-data-stream shaped component resets too ("a:" is
        // a root-name).
        const std::string tar = raw_tar_entry("data/" + other + "evil.txt", "boom");
        std::size_t pos = 0;
        EXPECT_THROW(extract_tar_to_dir(source_from(tar, &pos), dest.path()), DockerError);
    }
#else
    const std::string tar = raw_tar_entry("data/C:/evil.txt", "boom");
    std::size_t pos = 0;
    extract_tar_to_dir(source_from(tar, &pos), dest.path());
    EXPECT_EQ(slurp(dest.path() / "data" / "C:" / "evil.txt"), "boom");
#endif
}

TEST(Tar, ExtractToDirSkipsSymlinks) {
    // A symlink-only archive: the entry is skipped by policy (a written link
    // could redirect later entries outside dest), nothing is created.
    const std::string tar = raw_tar_entry("evil-link", "", /*symlink_target=*/"/etc");
    const DestDir dest;
    std::size_t pos = 0;
    extract_tar_to_dir(source_from(tar, &pos), dest.path());
    EXPECT_FALSE(std::filesystem::exists(dest.path() / "evil-link"));
    EXPECT_TRUE(std::filesystem::is_directory(dest.path())); // dest itself was created

    // A subsequent normal archive still extracts into the same dest.
    const std::string normal = build_context_tar({TarFile{"kept.txt", "kept"}});
    std::size_t pos2 = 0;
    extract_tar_to_dir(source_from(normal, &pos2), dest.path());
    EXPECT_EQ(slurp(dest.path() / "kept.txt"), "kept");
}

TEST(Tar, ExtractToDirSourceExceptionPropagates) {
    const DestDir dest;
    const TarSource boom = [](char*, std::size_t) -> std::size_t {
        throw std::runtime_error("download died");
    };
    try {
        extract_tar_to_dir(boom, dest.path());
        FAIL() << "extract_tar_to_dir did not propagate the source exception";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "download died");
    }
}

TEST(Tar, ContextTarTwoFiles) {
    const std::vector<TarFile> files = {
        TarFile{"Dockerfile", "FROM alpine:3.20\n"},
        TarFile{"app/x.txt", "hello-context", 0600},
    };
    const std::string tar = build_context_tar(files);
    const std::vector<TarEntry> entries = extract_tar(tar);

    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].name, "Dockerfile");
    EXPECT_EQ(entries[0].body, "FROM alpine:3.20\n");
    EXPECT_TRUE(entries[0].is_regular_file);
    EXPECT_EQ(entries[0].mode & 0777, 0644); // default mode

    EXPECT_EQ(entries[1].name, "app/x.txt");
    EXPECT_EQ(entries[1].body, "hello-context");
    EXPECT_TRUE(entries[1].is_regular_file);
    EXPECT_EQ(entries[1].mode & 0777, 0600);
}
