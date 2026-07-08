#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "CannedHttpServer.hpp"

#if defined(_WIN32)
#include <process.h>
#endif

// Helpers shared by the unit-test translation units: the Docker stream-frame
// encoder, the canned HTTP responses every DockerClient test scripts, and
// small assertion predicates. One copy here instead of one per test file.
namespace tcunit {

/// One multiplexed stream frame: 8-byte header {kind, 0, 0, 0, len_be32} +
/// payload — Docker's wire format for non-TTY output (kind: 0 stdin,
/// 1 stdout, 2 stderr).
inline std::string frame(unsigned char kind, std::string_view payload) {
    std::string f;
    f.push_back(static_cast<char>(kind));
    f.append(3, '\0');
    const auto len = static_cast<std::uint32_t>(payload.size());
    f.push_back(static_cast<char>((len >> 24) & 0xFF));
    f.push_back(static_cast<char>((len >> 16) & 0xFF));
    f.push_back(static_cast<char>((len >> 8) & 0xFF));
    f.push_back(static_cast<char>(len & 0xFF));
    f.append(payload);
    return f;
}

/// The API-version negotiation `GET /_ping` every fresh client issues before
/// its first typed call. No Api-Version header on purpose: the client then
/// falls back to unversioned paths, so path assertions stay
/// version-independent (the pinning itself is covered by ApiVersionTest).
inline std::string ping_ok() { return http_response(200, "OK", "OK"); }

/// A 201 reply carrying the new resource id — the shape of the
/// container-create and exec-create responses.
inline std::string created(const std::string& id) {
    return http_response(201, "Created", R"({"Id":")" + id + R"("})");
}

/// True when the recorded request head starts with "<METHOD> <path-prefix>".
inline bool request_is(const std::string& head, const std::string& method_and_path) {
    return head.rfind(method_and_path, 0) == 0;
}

/// True if `haystack` contains `needle`.
inline bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

/// True if `seq` contains `value` anywhere.
inline bool contains(const std::vector<std::string>& seq, const std::string& value) {
    return std::find(seq.begin(), seq.end(), value) != seq.end();
}

/// True when `s` consists entirely of lowercase hex digits.
inline bool is_lower_hex(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

#if defined(_WIN32)
/// A per-process unique local pipe name: \\.\pipe\<tag>-<pid> (two test
/// processes sharing a machine must never collide on a pipe).
inline std::string pipe_name(const std::string& tag) {
    return R"(\\.\pipe\)" + tag + "-" + std::to_string(::_getpid());
}

/// The npipe:// DockerHost for the same pipe (forward slashes in the URL; the
/// transport converts back to backslashes).
inline testcontainers::DockerHost pipe_host(const std::string& tag) {
    return testcontainers::DockerHost::parse("npipe:////./pipe/" + tag + "-" +
                                             std::to_string(::_getpid()));
}
#endif

} // namespace tcunit
