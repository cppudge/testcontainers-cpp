#pragma once

#include <string>
#include <vector>

// .dockerignore parsing and matching with Go (moby/patternmatcher) semantics,
// so a directory build context filtered here matches what `docker build`
// would send. Free of Boost/libarchive so it unit-tests standalone.
namespace testcontainers::docker {

/// One parsed .dockerignore pattern: cleaned '/'-separated components plus
/// the negation flag ("!pattern" re-includes what an earlier pattern
/// excluded).
struct IgnorePattern {
    std::vector<std::string> components;
    bool negated = false;
};

/// Parse .dockerignore text: one pattern per line; blank lines and lines
/// starting with '#' are skipped; a leading '!' negates; patterns are
/// trimmed, leading "/" and "./" dropped, trailing '/' dropped (a directory
/// pattern and its bare name are equivalent).
std::vector<IgnorePattern> parse_dockerignore(const std::string& text);

/// True when `rel_path` (a '/'-separated path relative to the context root,
/// no leading or trailing slash) is EXCLUDED by `patterns`. docker-build
/// (moby/patternmatcher) semantics: each component matches with glob rules
/// ('*' within a component, '?', '[a-z]' classes — '^' inside a class is a
/// LITERAL member, not negation, because moby literalizes it), '\' escapes,
/// "**" spans any number of components, a pattern matching a parent
/// directory excludes everything under it, and the LAST matching pattern
/// decides (so a later "!pattern" re-includes). A malformed class matches
/// nothing where docker build would reject the whole pattern.
bool dockerignore_excludes(const std::vector<IgnorePattern>& patterns, const std::string& rel_path);

} // namespace testcontainers::docker
