#include <gtest/gtest.h>

#include <archive.h>
#include <archive_entry.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
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

using testcontainers::CopyToContainer;
using testcontainers::DockerError;
using testcontainers::docker::build_context_tar;
using testcontainers::docker::build_tar;
using testcontainers::docker::extract_tar;
using testcontainers::docker::strip_leading_slash;
using testcontainers::docker::TarEntry;
using testcontainers::docker::TarFile;

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
