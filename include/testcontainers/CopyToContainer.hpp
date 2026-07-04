#pragma once

#include <filesystem>
#include <string>
#include <utility>

namespace testcontainers {

/// A request to copy a file or in-memory bytes into a container, mapping to a
/// single-entry tar `PUT`ed to `PUT /containers/{id}/archive`.
///
/// Two sources: a host file (read at copy time) or raw in-memory bytes (which
/// may be binary, including embedded NULs). Build one with a static factory,
/// then optionally tune the file mode with the chainable setter
/// (same style as `Mount`/`Healthcheck`).
///
/// A plain, copyable value type (no Boost/Asio/libarchive leakage).
class CopyToContainer {
public:
    /// Copy a host file (`host_path` on the local filesystem) to `container_target`
    /// (an absolute path in the container, e.g. "/tmp/foo.txt"). The file is read
    /// when the copy runs. `container_target` is a container-side path and stays a
    /// plain string (always '/'-separated, independent of the host OS).
    static CopyToContainer host_file(std::filesystem::path host_path,
                                     std::string container_target) {
        CopyToContainer c;
        c.is_file_ = true;
        c.host_path_ = std::move(host_path);
        c.target_ = std::move(container_target);
        return c;
    }

    /// Copy in-memory bytes to `container_target`. `bytes` is taken verbatim and
    /// may be binary (embedded NULs are preserved).
    static CopyToContainer content(std::string bytes, std::string container_target) {
        CopyToContainer c;
        c.is_file_ = false;
        c.bytes_ = std::move(bytes);
        c.target_ = std::move(container_target);
        return c;
    }

    // --- In-place setters (single overload; chain on lvalues and temporaries) ---

    /// Set the octal file mode of the created entry (e.g. `0644`). Defaults to
    /// `0644` when unset.
    CopyToContainer& with_mode(int mode) {
        mode_ = mode;
        return *this;
    }

    // --- Getters ---

    /// True if the source is a host file; false if it is in-memory bytes.
    bool is_file() const noexcept { return is_file_; }
    const std::filesystem::path& host_path() const noexcept { return host_path_; }
    const std::string& bytes() const noexcept { return bytes_; }
    const std::string& target() const noexcept { return target_; }
    int mode() const noexcept { return mode_; }

private:
    bool is_file_ = false;
    std::filesystem::path host_path_; ///< set when is_file_ (host filesystem path)
    std::string bytes_;               ///< set when !is_file_ (may be binary)
    std::string target_;              ///< absolute path inside the container
    int mode_ = 0644;                 ///< octal regular-file mode
};

} // namespace testcontainers
