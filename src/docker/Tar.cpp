#include "docker/Tar.hpp"

#include "testcontainers/Error.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
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

/// Read the whole host file into a string (binary), or throw DockerError.
std::string read_host_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw DockerError("copy_to_container: cannot open host file '" + path.string() + "'");
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw DockerError("copy_to_container: failed reading host file '" + path.string() + "'");
    }
    return data;
}

} // namespace

std::string strip_leading_slash(const std::string& path) {
    if (!path.empty() && path.front() == '/') {
        return path.substr(1);
    }
    return path;
}

std::string build_tar(const CopyToContainer& source) {
    // The entry body: either the host file's bytes or the in-memory bytes.
    const std::string body = source.is_file() ? read_host_file(source.host_path()) : source.bytes();
    const std::string name = strip_leading_slash(source.target());

    struct archive* a = archive_write_new();
    if (a == nullptr) {
        throw DockerError("copy_to_container: archive_write_new failed");
    }
    // USTAR is the simplest portable format Docker accepts for a single file.
    archive_write_set_format_ustar(a);

    std::string out;
    if (archive_write_open(a, &out, /*open*/ nullptr, append_to_string, /*close*/ nullptr) !=
        ARCHIVE_OK) {
        const std::string err = archive_error_string(a) ? archive_error_string(a) : "unknown error";
        archive_write_free(a);
        throw DockerError("copy_to_container: archive_write_open failed: " + err);
    }

    struct archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname(entry, name.c_str());
    archive_entry_set_filetype(entry, AE_IFREG);
    // libarchive's mode parameter is 16-bit on Windows (CRT _mode_t);
    // permission bits always fit.
    archive_entry_set_perm(entry, static_cast<unsigned short>(source.mode()));
    archive_entry_set_size(entry, static_cast<la_int64_t>(body.size()));

    if (archive_write_header(a, entry) != ARCHIVE_OK) {
        const std::string err = archive_error_string(a) ? archive_error_string(a) : "unknown error";
        archive_entry_free(entry);
        archive_write_free(a);
        throw DockerError("copy_to_container: archive_write_header failed: " + err);
    }
    if (!body.empty()) {
        const la_ssize_t written = archive_write_data(a, body.data(), body.size());
        if (written < 0 || static_cast<std::size_t>(written) != body.size()) {
            const std::string err =
                archive_error_string(a) ? archive_error_string(a) : "short write";
            archive_entry_free(entry);
            archive_write_free(a);
            throw DockerError("copy_to_container: archive_write_data failed: " + err);
        }
    }

    archive_entry_free(entry);
    // archive_write_close flushes the trailing blocks; archive_write_free closes
    // implicitly but we close explicitly to surface any flush error.
    if (archive_write_close(a) != ARCHIVE_OK) {
        const std::string err = archive_error_string(a) ? archive_error_string(a) : "unknown error";
        archive_write_free(a);
        throw DockerError("copy_to_container: archive_write_close failed: " + err);
    }
    archive_write_free(a);
    return out;
}

std::string build_context_tar(const std::vector<TarFile>& files) {
    struct archive* a = archive_write_new();
    if (a == nullptr) {
        throw DockerError("build_context_tar: archive_write_new failed");
    }
    // USTAR is the simplest portable format Docker accepts for a build context.
    archive_write_set_format_ustar(a);

    std::string out;
    if (archive_write_open(a, &out, /*open*/ nullptr, append_to_string, /*close*/ nullptr) !=
        ARCHIVE_OK) {
        const std::string err = archive_error_string(a) ? archive_error_string(a) : "unknown error";
        archive_write_free(a);
        throw DockerError("build_context_tar: archive_write_open failed: " + err);
    }

    for (const TarFile& file : files) {
        const std::string name = strip_leading_slash(file.name);

        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        // 16-bit on Windows, see above.
        archive_entry_set_perm(entry, static_cast<unsigned short>(file.mode));
        archive_entry_set_size(entry, static_cast<la_int64_t>(file.body.size()));

        if (archive_write_header(a, entry) != ARCHIVE_OK) {
            const std::string err =
                archive_error_string(a) ? archive_error_string(a) : "unknown error";
            archive_entry_free(entry);
            archive_write_free(a);
            throw DockerError("build_context_tar: archive_write_header failed: " + err);
        }
        if (!file.body.empty()) {
            const la_ssize_t written = archive_write_data(a, file.body.data(), file.body.size());
            if (written < 0 || static_cast<std::size_t>(written) != file.body.size()) {
                const std::string err =
                    archive_error_string(a) ? archive_error_string(a) : "short write";
                archive_entry_free(entry);
                archive_write_free(a);
                throw DockerError("build_context_tar: archive_write_data failed: " + err);
            }
        }
        archive_entry_free(entry);
    }

    // archive_write_close flushes the trailing blocks; archive_write_free closes
    // implicitly but we close explicitly to surface any flush error.
    if (archive_write_close(a) != ARCHIVE_OK) {
        const std::string err = archive_error_string(a) ? archive_error_string(a) : "unknown error";
        archive_write_free(a);
        throw DockerError("build_context_tar: archive_write_close failed: " + err);
    }
    archive_write_free(a);
    return out;
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
