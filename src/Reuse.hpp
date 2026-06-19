#pragma once

#include <string>

namespace testcontainers::detail {

/// The label whose value is a container's reuse hash. Reusable containers carry
/// this (instead of the session-id label) so a later run can find and adopt a
/// still-running container that matches the same config.
const char* reuse_hash_label();

/// A stable (cross-run, cross-platform) hash of a container's canonical config,
/// used as the reuse-match label value. Deterministic; NOT cryptographic — a
/// 64-bit FNV-1a over `canonical`, rendered as 16 lowercase hex chars. (We only
/// need OUR containers to match OUR hash; no interop with other testcontainers.)
std::string reuse_hash(const std::string& canonical);

/// Whether container reuse is enabled: env TESTCONTAINERS_REUSE_ENABLE in
/// {1,true,TRUE,True}, else `testcontainers.reuse.enable=true` in
/// ~/.testcontainers.properties (HOME or USERPROFILE). Default false.
bool reuse_enabled();

} // namespace testcontainers::detail
