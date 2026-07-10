#include "docker/DockerIgnore.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace testcontainers::docker {

namespace {

/// Trim ASCII whitespace from both ends.
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

/// Split a cleaned pattern into '/'-separated components, dropping empty ones
/// (doubled slashes) and "." (current-dir no-ops).
std::vector<std::string> split_components(std::string_view s) {
    std::vector<std::string> out;
    while (!s.empty()) {
        const std::size_t sep = s.find('/');
        const std::string_view part = s.substr(0, sep);
        if (!part.empty() && part != ".") {
            out.emplace_back(part);
        }
        if (sep == std::string_view::npos) {
            break;
        }
        s.remove_prefix(sep + 1);
    }
    return out;
}

/// Glob-match ONE path component the way docker's matcher does: '*' matches
/// any run of chars (never crossing a '/'; components are pre-split so that
/// is implicit), '?' one char, '[a-z]' classes with ranges ('^' is a literal
/// member, not negation — see below), '\' escapes the next char. Malformed
/// classes match nothing (docker build rejects such patterns outright; a
/// non-match is the softer equivalent here).
bool match_component(std::string_view pattern, std::string_view name) {
    std::size_t p = 0;
    std::size_t n = 0;
    // Backtracking state for the most recent '*'.
    std::size_t star_p = std::string_view::npos;
    std::size_t star_n = 0;

    while (n < name.size()) {
        bool advance = false;
        if (p < pattern.size()) {
            const char pc = pattern[p];
            if (pc == '*') {
                star_p = ++p; // try zero chars first; retry points eat one more
                star_n = n;
                continue;
            }
            if (pc == '?') {
                ++p;
                ++n;
                continue;
            }
            if (pc == '[') {
                // Character class: [abc], [a-z]. NO negation on purpose:
                // docker's matcher (moby/patternmatcher) literalizes every
                // class member through regexp.QuoteMeta, so "[^a]" is the
                // POSITIVE class {'^','a'} — mirror that, not filepath.Match.
                std::size_t c = p + 1;
                bool matched = false;
                bool closed = false;
                bool first = true;
                while (c < pattern.size()) {
                    if (pattern[c] == ']' && !first) {
                        closed = true;
                        break;
                    }
                    first = false;
                    char lo = pattern[c];
                    if (lo == '\\' && c + 1 < pattern.size()) {
                        lo = pattern[++c];
                    }
                    char hi = lo;
                    if (c + 2 < pattern.size() && pattern[c + 1] == '-' && pattern[c + 2] != ']') {
                        hi = pattern[c + 2];
                        if (hi == '\\' && c + 3 < pattern.size()) {
                            hi = pattern[c + 3];
                            ++c;
                        }
                        c += 2;
                    }
                    if (name[n] >= lo && name[n] <= hi) {
                        matched = true;
                    }
                    ++c;
                }
                if (closed && matched) {
                    p = c + 1;
                    ++n;
                    continue;
                }
                // No match, or a malformed (unterminated) class. docker build
                // would REJECT a malformed pattern outright; matching nothing
                // is the softer equivalent here.
                advance = true;
            } else {
                char literal = pc;
                if (literal == '\\' && p + 1 < pattern.size()) {
                    literal = pattern[p + 1];
                    if (literal == name[n]) {
                        p += 2;
                        ++n;
                        continue;
                    }
                    advance = true;
                } else if (literal == name[n]) {
                    ++p;
                    ++n;
                    continue;
                } else {
                    advance = true;
                }
            }
        } else {
            advance = true;
        }
        if (advance) {
            // Mismatch: give the last '*' one more character, or fail.
            if (star_p == std::string_view::npos) {
                return false;
            }
            p = star_p;
            n = ++star_n;
        }
    }
    // Name consumed: the rest of the pattern must be all '*'.
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

/// Match pattern components (with "**" spanning any number of path
/// components) against path components [vi, path_end), both fully.
/// `path_end` caps the path so parent-prefix checks need no copies.
bool match_from(const std::vector<std::string>& pattern, std::size_t pi,
                const std::vector<std::string>& path, std::size_t vi, std::size_t path_end) {
    if (pi == pattern.size()) {
        return vi == path_end;
    }
    if (pattern[pi] == "**") {
        // Zero components, or eat one and stay on the same "**".
        if (match_from(pattern, pi + 1, path, vi, path_end)) {
            return true;
        }
        return vi < path_end && match_from(pattern, pi, path, vi + 1, path_end);
    }
    if (vi == path_end) {
        return false;
    }
    return match_component(pattern[pi], path[vi]) &&
           match_from(pattern, pi + 1, path, vi + 1, path_end);
}

/// Go's MatchesOrParentMatches: the pattern excludes `path` when it matches
/// the path itself OR any parent directory of it ("node_modules" excludes
/// "node_modules/a/b.js").
bool matches_or_parent(const IgnorePattern& pattern, const std::vector<std::string>& path) {
    for (std::size_t take = 1; take <= path.size(); ++take) {
        if (match_from(pattern.components, 0, path, 0, take)) {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<IgnorePattern> parse_dockerignore(const std::string& text) {
    std::vector<IgnorePattern> out;
    std::string_view rest(text);
    while (!rest.empty()) {
        const std::size_t nl = rest.find('\n');
        std::string_view line = trim(rest.substr(0, nl));
        rest = (nl == std::string_view::npos) ? std::string_view{} : rest.substr(nl + 1);

        if (line.empty() || line.front() == '#') {
            continue;
        }
        IgnorePattern pattern;
        if (line.front() == '!') {
            pattern.negated = true;
            line = trim(line.substr(1));
            if (line.empty()) {
                continue;
            }
        }
        // Leading "/" and "./" are no-ops (patterns are context-relative);
        // a trailing "/" makes a directory pattern equal to its bare name.
        while (line.size() >= 2 && line.substr(0, 2) == "./") {
            line = line.substr(2);
        }
        while (!line.empty() && line.front() == '/') {
            line = line.substr(1);
        }
        while (!line.empty() && line.back() == '/') {
            line = line.substr(0, line.size() - 1);
        }
        pattern.components = split_components(line);
        if (!pattern.components.empty()) {
            out.push_back(std::move(pattern));
        }
    }
    return out;
}

bool dockerignore_excludes(const std::vector<IgnorePattern>& patterns,
                           const std::string& rel_path) {
    const std::vector<std::string> path = split_components(rel_path);
    if (path.empty()) {
        return false;
    }
    // Every pattern is consulted in order; the LAST one that matches decides
    // (a later "!pattern" re-includes what an earlier pattern excluded).
    bool excluded = false;
    for (const IgnorePattern& pattern : patterns) {
        if (matches_or_parent(pattern, path)) {
            excluded = !pattern.negated;
        }
    }
    return excluded;
}

} // namespace testcontainers::docker
