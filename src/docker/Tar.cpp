#include "docker/Tar.hpp"

#include "testcontainers/Error.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cstddef>
#include <fstream>
#include <ios>
#include <string>

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
std::string read_host_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw DockerError("copy_to_container: cannot open host file '" + path + "'");
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw DockerError("copy_to_container: failed reading host file '" + path + "'");
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
    archive_entry_set_perm(entry, static_cast<int>(source.mode()));
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

} // namespace testcontainers::docker
