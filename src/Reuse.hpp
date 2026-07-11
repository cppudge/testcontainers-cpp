#pragma once

#include <string>

namespace testcontainers::detail {

/// The label whose value is a resource's reuse hash. Reusable containers and
/// networks carry this (instead of the session-id label) so a later run can
/// find and adopt an existing resource that matches the same config.
const char* reuse_hash_label();

/// A stable (cross-run, cross-platform) hash of a resource's canonical config,
/// used as the reuse-match label value. Deterministic; NOT cryptographic — a
/// 64-bit FNV-1a over `canonical`, rendered as 16 lowercase hex chars. (We only
/// need OUR resources to match OUR hash; no interop with other testcontainers.)
std::string reuse_hash(const std::string& canonical);

/// Whether container reuse is enabled: env TESTCONTAINERS_REUSE_ENABLE when
/// set (non-empty) decides — in {1,true,TRUE,True}, so an explicit "false"
/// overrides a file-enabled switch — else `testcontainers.reuse.enable=true`
/// (value case-insensitive, java Boolean.parseBoolean parity) in
/// ~/.testcontainers.properties, read once per process. Default false.
bool reuse_enabled();

} // namespace testcontainers::detail
