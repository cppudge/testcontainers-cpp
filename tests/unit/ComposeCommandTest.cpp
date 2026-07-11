#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "TestSupport.hpp"
#include "compose/ComposeCommand.hpp"

// Tests in this file (pure, daemon-free arg-builder assertions):
//   ComposeCommand.UpArgsProjectNameAndFiles - up args start with --project-name <p> and emit -f <file> for every file.
//   ComposeCommand.UpArgsHaveUpDashD - up args contain `up` followed by `-d`.
//   ComposeCommand.UpArgsWaitByDefault - with wait on, up args contain --wait --wait-timeout <n>.
//   ComposeCommand.UpArgsNoWaitWhenDisabled - with wait off, up args contain neither --wait nor --wait-timeout.
//   ComposeCommand.UpArgsBuildAndPull - --build and --pull always appear only when requested.
//   ComposeCommand.UpArgsProfilesBeforeUp - each profile is emitted as `--profile <name>` (in order, before `up`); none by default.
//   ComposeCommand.UpArgsScaleAfterUp - each scale is emitted as `--scale <service>=<n>` after `up`; none by default.
//   ComposeCommand.DownArgsBare - a bare down emits --project-name <p> down and (by default) no extras here.
//   ComposeCommand.DownArgsProfilesBeforeDown - down carries the same `--profile <name>` flags, before `down`.
//   ComposeCommand.DownArgsVolumesAndRmi - --volumes and --rmi all appear only when requested (rmi takes the `all` argument).
//   ComposeCommand.ShellQuoteWrapsInSingleQuotes - plain tokens and ones with spaces/$/" are wrapped in single quotes verbatim.
//   ComposeCommand.ShellQuoteEscapesEmbeddedSingleQuotes - an embedded ' becomes the '\'' close-escape-reopen sequence.
//   ComposeCommand.ShellQuoteAssignment - KEY='value' assignments quote the value only.
//   ComposeCommand.EnvWrappedScriptPrefixesAssignments - build_env_wrapped_script emits the env assignments before the quoted argv, all shell-quoted (spaces/quotes survive).
//   ComposeCommand.EnvWrappedScriptEmptyEnvIsJustArgv - with no env the script is just the quoted argv.

using namespace testcontainers::compose;

namespace {

using tcunit::contains;

// Index of the first occurrence of `value`, or -1 if absent.
int index_of(const std::vector<std::string>& seq, const std::string& value) {
    const auto it = std::find(seq.begin(), seq.end(), value);
    return it == seq.end() ? -1 : static_cast<int>(it - seq.begin());
}

// Count occurrences of `value`.
int count(const std::vector<std::string>& seq, const std::string& value) {
    return static_cast<int>(std::count(seq.begin(), seq.end(), value));
}

} // namespace

TEST(ComposeCommand, UpArgsProjectNameAndFiles) {
    ComposeUpCommand up;
    up.project_name = "proj";
    up.files = {"/a.yml", "/b.yml"};
    const std::vector<std::string> args = build_compose_up_args(up);

    // Starts at --project-name <p>.
    ASSERT_GE(args.size(), 2u);
    EXPECT_EQ(args[0], "--project-name");
    EXPECT_EQ(args[1], "proj");

    // Each file emitted as `-f <file>`, in order.
    EXPECT_EQ(count(args, "-f"), 2);
    const int fa = index_of(args, "/a.yml");
    const int fb = index_of(args, "/b.yml");
    ASSERT_GT(fa, 0);
    ASSERT_GT(fb, 0);
    EXPECT_EQ(args[fa - 1], "-f");
    EXPECT_EQ(args[fb - 1], "-f");
    EXPECT_LT(fa, fb);
}

TEST(ComposeCommand, UpArgsHaveUpDashD) {
    ComposeUpCommand up;
    up.project_name = "proj";
    const std::vector<std::string> args = build_compose_up_args(up);

    const int up_idx = index_of(args, "up");
    ASSERT_GE(up_idx, 0);
    ASSERT_LT(static_cast<std::size_t>(up_idx) + 1, args.size());
    EXPECT_EQ(args[up_idx + 1], "-d");
}

TEST(ComposeCommand, UpArgsWaitByDefault) {
    ComposeUpCommand up;
    up.project_name = "proj";
    up.wait = true;
    up.wait_timeout_secs = 90;
    const std::vector<std::string> args = build_compose_up_args(up);

    EXPECT_TRUE(contains(args, "--wait"));
    const int wt = index_of(args, "--wait-timeout");
    ASSERT_GT(wt, 0);
    ASSERT_LT(static_cast<std::size_t>(wt) + 1, args.size());
    EXPECT_EQ(args[wt + 1], "90");
}

TEST(ComposeCommand, UpArgsNoWaitWhenDisabled) {
    ComposeUpCommand up;
    up.project_name = "proj";
    up.wait = false;
    const std::vector<std::string> args = build_compose_up_args(up);

    EXPECT_FALSE(contains(args, "--wait"));
    EXPECT_FALSE(contains(args, "--wait-timeout"));
}

TEST(ComposeCommand, UpArgsBuildAndPull) {
    {
        ComposeUpCommand up;
        up.project_name = "proj";
        up.build = false;
        up.pull = false;
        const std::vector<std::string> args = build_compose_up_args(up);
        EXPECT_FALSE(contains(args, "--build"));
        EXPECT_FALSE(contains(args, "--pull"));
    }
    {
        ComposeUpCommand up;
        up.project_name = "proj";
        up.build = true;
        up.pull = true;
        const std::vector<std::string> args = build_compose_up_args(up);
        EXPECT_TRUE(contains(args, "--build"));
        const int pull = index_of(args, "--pull");
        ASSERT_GT(pull, 0);
        ASSERT_LT(static_cast<std::size_t>(pull) + 1, args.size());
        EXPECT_EQ(args[pull + 1], "always");
    }
}

TEST(ComposeCommand, UpArgsProfilesBeforeUp) {
    {
        ComposeUpCommand up;
        up.project_name = "proj";
        const std::vector<std::string> args = build_compose_up_args(up);
        EXPECT_FALSE(contains(args, "--profile"));
    }
    {
        ComposeUpCommand up;
        up.project_name = "proj";
        up.profiles = {"frontend", "debug"};
        const std::vector<std::string> args = build_compose_up_args(up);

        // `--profile <name>` per profile, in order, and — being a top-level
        // compose flag — before the `up` subcommand.
        EXPECT_EQ(count(args, "--profile"), 2);
        const int fe = index_of(args, "frontend");
        const int dbg = index_of(args, "debug");
        ASSERT_GT(fe, 0);
        ASSERT_GT(dbg, 0);
        EXPECT_EQ(args[fe - 1], "--profile");
        EXPECT_EQ(args[dbg - 1], "--profile");
        EXPECT_LT(fe, dbg);
        EXPECT_LT(dbg, index_of(args, "up"));
    }
}

TEST(ComposeCommand, UpArgsScaleAfterUp) {
    {
        ComposeUpCommand up;
        up.project_name = "proj";
        const std::vector<std::string> args = build_compose_up_args(up);
        EXPECT_FALSE(contains(args, "--scale"));
    }
    {
        ComposeUpCommand up;
        up.project_name = "proj";
        up.scales = {{"redis", 2}, {"worker", 3}};
        const std::vector<std::string> args = build_compose_up_args(up);

        // `--scale <service>=<n>` per service — an `up` flag, so after the
        // subcommand (unlike the top-level --profile/-f globals).
        EXPECT_EQ(count(args, "--scale"), 2);
        const int redis = index_of(args, "redis=2");
        const int worker = index_of(args, "worker=3");
        ASSERT_GT(redis, 0);
        ASSERT_GT(worker, 0);
        EXPECT_EQ(args[redis - 1], "--scale");
        EXPECT_EQ(args[worker - 1], "--scale");
        EXPECT_GT(redis, index_of(args, "up"));
    }
}

TEST(ComposeCommand, DownArgsBare) {
    ComposeDownCommand down;
    down.project_name = "proj";
    down.volumes = false;
    down.remove_images = false;
    const std::vector<std::string> args = build_compose_down_args(down);

    ASSERT_GE(args.size(), 3u);
    EXPECT_EQ(args[0], "--project-name");
    EXPECT_EQ(args[1], "proj");
    EXPECT_TRUE(contains(args, "down"));
    EXPECT_FALSE(contains(args, "--volumes"));
    EXPECT_FALSE(contains(args, "--rmi"));
}

TEST(ComposeCommand, DownArgsProfilesBeforeDown) {
    ComposeDownCommand down;
    down.project_name = "proj";
    down.profiles = {"frontend"};
    const std::vector<std::string> args = build_compose_down_args(down);

    // The teardown must see the same profiles as `up`, or compose skips the
    // profile-gated services when enumerating what to remove.
    const int fe = index_of(args, "frontend");
    ASSERT_GT(fe, 0);
    EXPECT_EQ(args[fe - 1], "--profile");
    EXPECT_LT(fe, index_of(args, "down"));
}

TEST(ComposeCommand, DownArgsVolumesAndRmi) {
    ComposeDownCommand down;
    down.project_name = "proj";
    down.volumes = true;
    down.remove_images = true;
    const std::vector<std::string> args = build_compose_down_args(down);

    EXPECT_TRUE(contains(args, "down"));
    EXPECT_TRUE(contains(args, "--volumes"));
    const int rmi = index_of(args, "--rmi");
    ASSERT_GT(rmi, 0);
    ASSERT_LT(static_cast<std::size_t>(rmi) + 1, args.size());
    EXPECT_EQ(args[rmi + 1], "all"); // compose v2 requires the `all` argument
}

TEST(ComposeCommand, ShellQuoteWrapsInSingleQuotes) {
    EXPECT_EQ(shell_quote("plain"), "'plain'");
    EXPECT_EQ(shell_quote("with spaces"), "'with spaces'");
    // Inside single quotes /bin/sh treats $ and " literally — no extra escaping.
    EXPECT_EQ(shell_quote("$HOME \"x\""), "'$HOME \"x\"'");
    EXPECT_EQ(shell_quote(""), "''");
}

TEST(ComposeCommand, ShellQuoteEscapesEmbeddedSingleQuotes) {
    // ' closes the quote, \' emits a literal quote, ' reopens: 'it'\''s'.
    EXPECT_EQ(shell_quote("it's"), "'it'\\''s'");
    EXPECT_EQ(shell_quote("''"), "''\\'''\\'''");
}

TEST(ComposeCommand, ShellQuoteAssignment) {
    EXPECT_EQ(shell_quote_assignment("KEY", "value"), "KEY='value'");
    EXPECT_EQ(shell_quote_assignment("MSG", "hello world"), "MSG='hello world'");
    EXPECT_EQ(shell_quote_assignment("Q", "a'b"), "Q='a'\\''b'");
}

TEST(ComposeCommand, EnvWrappedScriptPrefixesAssignments) {
    // The exact /bin/sh -c script the containerised client execs: env
    // assignments first, then every argv token, all shell-quoted so values
    // with spaces and quotes survive the shell.
    const std::string script =
        build_env_wrapped_script({"docker", "compose", "up"}, {{"FOO", "a b"}, {"Q", "it's"}});
    EXPECT_EQ(script, "FOO='a b' Q='it'\\''s' 'docker' 'compose' 'up'");
}

TEST(ComposeCommand, EnvWrappedScriptEmptyEnvIsJustArgv) {
    EXPECT_EQ(build_env_wrapped_script({"docker", "compose", "version"}, {}),
              "'docker' 'compose' 'version'");
}
