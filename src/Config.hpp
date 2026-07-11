#pragma once

#include <map>
#include <optional>
#include <string>

// Library configuration switches: an environment variable backed by a key in
// ~/.testcontainers.properties — the same file testcontainers-java reads, so
// the parsing rules below deliberately mirror Java where the two could
// diverge on a shared file. The pure parser is unit-testable on string
// bodies; the file read is cached for the process lifetime.
namespace testcontainers::detail {

/// Parse a Java-properties-style body into a key→value map: `key=value`
/// lines, '#'/'!' comment lines, keys and values trimmed of spaces/tabs/CR,
/// the LAST duplicate key wins (java.util.Properties#load order). Deliberate
/// simplifications (documented, matching what the file actually holds in the
/// wild): '=' is the only separator, and there are no escape sequences or
/// line continuations. Empty values are kept verbatim — the lookup layer
/// decides what "empty" means.
std::map<std::string, std::string> parse_properties(const std::string& body);

/// The value of `key` in ~/.testcontainers.properties (under HOME, else
/// USERPROFILE), read and parsed ONCE per process and cached; a missing or
/// unreadable file is an empty map. nullopt when the key is absent or its
/// value is empty.
std::optional<std::string> user_property(const std::string& key);

/// Test hook: drop the cached file contents so the next user_property()
/// re-reads ~/.testcontainers.properties. Not meant for library code — the
/// one-read-per-process cache is the documented behavior.
void clear_user_properties_cache();

/// The value of a configuration switch: environment variable `env_name` when
/// set and non-empty, else the `properties_key` entry of
/// ~/.testcontainers.properties. nullopt when neither provides a value.
std::optional<std::string> config_value(const char* env_name, const std::string& properties_key);

/// A boolean configuration switch with the same env-over-properties
/// precedence. A set (non-empty) environment variable DECIDES — via the
/// library's env set {1,true,TRUE,True} — even when it decides "false": an
/// explicit env off must be able to override a file-enabled switch (Java's
/// getEnvVarOrProperty behaves the same). Otherwise the properties value
/// decides with Boolean.parseBoolean parity: case-insensitive "true" only —
/// "1" is false there, and a value that enables a switch for
/// testcontainers-java must not silently do something else here. (On the ENV
/// side java applies parseBoolean too, so env "1" is false there; the docker-
/// style env set above is the library's established, deliberately lenient
/// superset — env vars are per-process, not a shared file, so the divergence
/// cannot flip one file's meaning between the two libraries.)
bool config_truthy(const char* env_name, const std::string& properties_key);

} // namespace testcontainers::detail
