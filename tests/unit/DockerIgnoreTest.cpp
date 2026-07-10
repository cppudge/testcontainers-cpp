#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "docker/DockerIgnore.hpp"

// Tests in this file:
//   DockerIgnore.ParseSkipsCommentsAndBlanks - '#' lines, blank lines, and lone "!" produce no patterns; whitespace is trimmed.
//   DockerIgnore.ParseNormalizesSlashes - leading "/" and "./" and a trailing "/" are dropped, so "/dir/", "./dir" and "dir" parse identically.
//   DockerIgnore.LiteralAndWildcards - the docker-docs examples: "*/temp*", "*/*/temp*", "temp?" match exactly the documented shapes.
//   DockerIgnore.DoubleStarSpansDirectories - "**/*.go" excludes .go files at any depth; "a/**/b" spans zero or more middle directories.
//   DockerIgnore.DirectoryPatternExcludesSubtree - "node_modules" excludes everything under node_modules/ (parent-directory match).
//   DockerIgnore.NegationLastMatchWins - the docs' "*.md" + "!README*.md" + "README-secret.md" ordering: README.md kept, README-secret.md excluded; reversing the last two flips the outcome.
//   DockerIgnore.NegationReincludesInsideExcludedDir - "logs" + "!logs/keep.txt" re-includes the one file (last match wins over the parent-dir exclusion).
//   DockerIgnore.CharacterClasses - "[a-c]?.txt" ranges match; '^' inside a class is a LITERAL member (docker literalizes it — no negation), pinning docker-build parity.
//   DockerIgnore.EscapedLiteral - "\*.txt" matches a literal '*' character, not the wildcard.

using testcontainers::docker::dockerignore_excludes;
using testcontainers::docker::IgnorePattern;
using testcontainers::docker::parse_dockerignore;

namespace {

/// Parse `text` and report whether `path` ends up excluded.
bool excludes(const std::string& text, const std::string& path) {
    return dockerignore_excludes(parse_dockerignore(text), path);
}

} // namespace

TEST(DockerIgnore, ParseSkipsCommentsAndBlanks) {
    const std::vector<IgnorePattern> patterns =
        parse_dockerignore("# a comment\n\n   \n!\n  *.log  \n");
    ASSERT_EQ(patterns.size(), 1u);
    EXPECT_FALSE(patterns[0].negated);
    ASSERT_EQ(patterns[0].components.size(), 1u);
    EXPECT_EQ(patterns[0].components[0], "*.log");
}

TEST(DockerIgnore, ParseNormalizesSlashes) {
    for (const std::string spelling : {"dir", "/dir", "./dir", "dir/", "/dir/"}) {
        EXPECT_TRUE(excludes(spelling, "dir/file.txt")) << spelling;
        EXPECT_TRUE(excludes(spelling, "dir")) << spelling;
        EXPECT_FALSE(excludes(spelling, "dirx/file.txt")) << spelling;
    }
}

TEST(DockerIgnore, LiteralAndWildcards) {
    // The examples from the Dockerfile reference.
    EXPECT_TRUE(excludes("*/temp*", "somedir/temporary.txt"));
    EXPECT_TRUE(excludes("*/temp*", "somedir/temp"));
    EXPECT_FALSE(excludes("*/temp*", "temp/foo.txt"));            // needs one leading dir
    EXPECT_FALSE(excludes("*/temp*", "somedir/subdir/temp.txt")); // exactly one level

    EXPECT_TRUE(excludes("*/*/temp*", "somedir/subdir/temporary.txt"));
    EXPECT_FALSE(excludes("*/*/temp*", "somedir/temporary.txt"));

    EXPECT_TRUE(excludes("temp?", "tempa"));
    EXPECT_TRUE(excludes("temp?", "tempb"));
    EXPECT_FALSE(excludes("temp?", "temp"));
    EXPECT_FALSE(excludes("temp?", "tempab"));

    EXPECT_TRUE(excludes("*.md", "README.md"));
    EXPECT_FALSE(excludes("*.md", "docs/README.md")); // '*' does not cross '/'
}

TEST(DockerIgnore, DoubleStarSpansDirectories) {
    EXPECT_TRUE(excludes("**/*.go", "main.go"));
    EXPECT_TRUE(excludes("**/*.go", "a/b/c/util.go"));
    EXPECT_FALSE(excludes("**/*.go", "main.rs"));

    EXPECT_TRUE(excludes("a/**/b", "a/b"));
    EXPECT_TRUE(excludes("a/**/b", "a/x/b"));
    EXPECT_TRUE(excludes("a/**/b", "a/x/y/b"));
    EXPECT_FALSE(excludes("a/**/b", "x/a/b"));

    // A parent-directory "**" pattern still excludes the subtree.
    EXPECT_TRUE(excludes("**/node_modules", "web/node_modules/pkg/index.js"));
}

TEST(DockerIgnore, DirectoryPatternExcludesSubtree) {
    EXPECT_TRUE(excludes("node_modules", "node_modules/left-pad/index.js"));
    EXPECT_TRUE(excludes("node_modules", "node_modules"));
    EXPECT_FALSE(excludes("node_modules", "src/node_modules.md"));
    EXPECT_FALSE(excludes("build/cache", "build/other/file"));
    EXPECT_TRUE(excludes("build/cache", "build/cache/deep/file"));
}

TEST(DockerIgnore, NegationLastMatchWins) {
    // The docs' canonical ordering: README-secret.md is excluded...
    const std::string keep_readme = "*.md\n!README*.md\nREADME-secret.md\n";
    EXPECT_FALSE(excludes(keep_readme, "README.md"));
    EXPECT_TRUE(excludes(keep_readme, "README-secret.md"));
    EXPECT_TRUE(excludes(keep_readme, "CHANGELOG.md"));

    // ...and with the last two swapped, every README* survives (the docs'
    // second example: the negation is now the last match for both).
    const std::string keep_all_readme = "*.md\nREADME-secret.md\n!README*.md\n";
    EXPECT_FALSE(excludes(keep_all_readme, "README.md"));
    EXPECT_FALSE(excludes(keep_all_readme, "README-secret.md"));
    EXPECT_TRUE(excludes(keep_all_readme, "CHANGELOG.md"));
}

TEST(DockerIgnore, NegationReincludesInsideExcludedDir) {
    const std::string text = "logs\n!logs/keep.txt\n";
    EXPECT_TRUE(excludes(text, "logs/debug.log"));
    EXPECT_FALSE(excludes(text, "logs/keep.txt"));
    EXPECT_TRUE(excludes(text, "logs"));
}

TEST(DockerIgnore, CharacterClasses) {
    EXPECT_TRUE(excludes("[a-c]?.txt", "a1.txt"));
    EXPECT_TRUE(excludes("[a-c]?.txt", "cz.txt"));
    EXPECT_FALSE(excludes("[a-c]?.txt", "d1.txt"));
    EXPECT_FALSE(excludes("[a-c]?.txt", "ab1.txt"));

    // docker (moby/patternmatcher) literalizes '^' inside a class instead of
    // negating: "[^a]" is the POSITIVE class {'^','a'} — parity pinned here.
    EXPECT_TRUE(excludes("[^a]*.log", "a.log"));
    EXPECT_TRUE(excludes("[^a]*.log", "^x.log"));
    EXPECT_FALSE(excludes("[^a]*.log", "b.log"));
}

TEST(DockerIgnore, EscapedLiteral) {
    EXPECT_TRUE(excludes("\\*.txt", "*.txt"));
    EXPECT_FALSE(excludes("\\*.txt", "a.txt"));
}
