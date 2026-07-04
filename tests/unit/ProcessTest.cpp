#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "Process.hpp"

// Tests in this file:
//   Process.QuoteArgWrapsInDoubleQuotes - a plain token and one with spaces are wrapped in double quotes verbatim.
//   Process.QuoteArgEscapesEmbeddedQuotes - an embedded double quote is backslash-escaped, with a preceding backslash run doubled.
//   Process.QuoteArgDoublesTrailingBackslashes - a trailing backslash run is doubled so the closing quote stays a delimiter; mid-string runs stay literal.
//   Process.BuildCommandLineJoinsQuotedArgs - the argv is quoted and space-joined (no shell, so no redirections or cmd.exe wrapping).
//   Process.RunProcessCapturesOutput - a real child process's stdout is captured with exit code 0.
//   Process.RunProcessCapturesStderr - the child's stderr lands in the same captured output (the merged-pipe contract).
//   Process.RunProcessReportsExitCode - a child exiting non-zero reports that exit code.
//   Process.RunProcessAppliesEnvWithoutTouchingParent - env overrides are visible to the child while the parent's environment stays untouched.
//   Process.RunProcessConcurrentEnvStaysIsolated - two concurrent runs overriding the SAME variable each see their own value (the explicit-env-block guarantee).
//   Process.RunProcessAppliesWorkingDir - working_dir becomes the child's working directory.
//   Process.RunProcessFeedsStdin - stdin_data reaches the child's stdin via the temp-file redirection.

using testcontainers::detail::build_command_line;
using testcontainers::detail::quote_arg;
using testcontainers::detail::run_process;

TEST(Process, QuoteArgWrapsInDoubleQuotes) {
    EXPECT_EQ(quote_arg("docker"), "\"docker\"");
    EXPECT_EQ(quote_arg("path with spaces/compose.yml"), "\"path with spaces/compose.yml\"");
    EXPECT_EQ(quote_arg(""), "\"\"");
}

TEST(Process, QuoteArgEscapesEmbeddedQuotes) {
    // CommandLineToArgvW / MSVCRT rules: the quote is escaped, and a backslash
    // run immediately before it is doubled (each backslash would otherwise eat
    // the escape).
    EXPECT_EQ(quote_arg("a\"b"), "\"a\\\"b\"");
    EXPECT_EQ(quote_arg("a\\\"b"), "\"a\\\\\\\"b\"");
}

TEST(Process, QuoteArgDoublesTrailingBackslashes) {
    // "C:\dir\" would swallow the closing quote; the trailing run is doubled.
    EXPECT_EQ(quote_arg("C:\\dir\\"), "\"C:\\dir\\\\\"");
    // A mid-string run not touching a quote is literal — NOT doubled.
    EXPECT_EQ(quote_arg("C:\\dir\\file"), "\"C:\\dir\\file\"");
}

TEST(Process, BuildCommandLineJoinsQuotedArgs) {
    // No shell between us and the child anymore: the line is exactly the
    // quoted argv — no `2>&1`, no cd prefix, no cmd.exe outer wrap.
    EXPECT_EQ(build_command_line({"docker", "compose", "version"}),
              "\"docker\" \"compose\" \"version\"");
}

// The end-to-end tests below run a real executable as the FIRST argv element
// ("exe + arguments" — the shape every real caller uses: docker, compose,
// docker-credential-<helper>). There is no shell in between, so shell builtins
// and .bat/.cmd scripts are out of scope by contract. PowerShell ships with
// every supported Windows; sh/echo/cat are POSIX-guaranteed.

TEST(Process, RunProcessCapturesOutput) {
#if defined(_WIN32)
    const auto result =
        run_process({"powershell", "-NoProfile", "-Command", "Write-Output 'hello world'"});
#else
    const auto result = run_process({"echo", "hello world"});
#endif
    EXPECT_EQ(result.exit_code, 0) << "output was: " << result.output;
    EXPECT_NE(result.output.find("hello world"), std::string::npos) << result.output;
}

TEST(Process, RunProcessCapturesStderr) {
    // Both std handles are wired to the same pipe (the popen-era `2>&1`
    // contract compose relies on for error messages).
#if defined(_WIN32)
    const auto result = run_process({"powershell", "-NoProfile", "-Command",
                                     "[Console]::Error.WriteLine('to-stderr'); exit 0"});
#else
    const auto result = run_process({"sh", "-c", "echo to-stderr 1>&2"});
#endif
    EXPECT_EQ(result.exit_code, 0) << "output was: " << result.output;
    EXPECT_NE(result.output.find("to-stderr"), std::string::npos) << result.output;
}

TEST(Process, RunProcessReportsExitCode) {
#if defined(_WIN32)
    const auto result = run_process({"powershell", "-NoProfile", "-Command", "exit 3"});
#else
    const auto result = run_process({"sh", "-c", "exit 3"});
#endif
    EXPECT_EQ(result.exit_code, 3);
}

namespace {

/// Run a child that prints the given environment variable.
testcontainers::detail::ProcessResult
child_print_env(const std::string& name,
                const std::vector<std::pair<std::string, std::string>>& env) {
#if defined(_WIN32)
    return run_process({"powershell", "-NoProfile", "-Command", "Write-Output $env:" + name},
                       /*working_dir*/ std::nullopt, env);
#else
    // printenv (not `sh -c 'echo $VAR'`): proving the child reads its OWN
    // environment, with no expansion layer in between.
    return run_process({"printenv", name}, /*working_dir*/ std::nullopt, env);
#endif
}

} // namespace

TEST(Process, RunProcessAppliesEnvWithoutTouchingParent) {
    ASSERT_EQ(std::getenv("TC_PROC_TEST_ENV"), nullptr)
        << "leftover env from a previous run; unset TC_PROC_TEST_ENV";

    const auto result =
        child_print_env("TC_PROC_TEST_ENV", {{"TC_PROC_TEST_ENV", "visible-in-child"}});
    EXPECT_EQ(result.exit_code, 0) << "output was: " << result.output;
    EXPECT_NE(result.output.find("visible-in-child"), std::string::npos) << result.output;
    // The override went into the CHILD's environment block only.
    EXPECT_EQ(std::getenv("TC_PROC_TEST_ENV"), nullptr);
}

TEST(Process, RunProcessConcurrentEnvStaysIsolated) {
    // Two threads override the SAME variable with different values — the
    // failure mode of the old save/apply/restore-on-the-global-environment
    // implementation (two compose stacks torn down concurrently could
    // cross-contaminate). With explicit per-child env blocks each child must
    // see exactly its own value, every round.
    ASSERT_EQ(std::getenv("TC_PROC_RACE_ENV"), nullptr)
        << "leftover env from a previous run; unset TC_PROC_RACE_ENV";

    constexpr int kRounds = 3;
    std::atomic<bool> mismatch{false};
    const auto worker = [&](const std::string& value) {
        for (int i = 0; i < kRounds && !mismatch; ++i) {
            const auto result = child_print_env("TC_PROC_RACE_ENV", {{"TC_PROC_RACE_ENV", value}});
            if (result.exit_code != 0 ||
                result.output.find(value) == std::string::npos) {
                mismatch = true;
            }
        }
    };
    std::thread alpha(worker, "value-alpha");
    std::thread bravo(worker, "value-bravo");
    alpha.join();
    bravo.join();
    EXPECT_FALSE(mismatch) << "a child saw the other run's value (env cross-contamination)";
    EXPECT_EQ(std::getenv("TC_PROC_RACE_ENV"), nullptr);
}

TEST(Process, RunProcessAppliesWorkingDir) {
    const std::string dir = std::filesystem::temp_directory_path().string();
#if defined(_WIN32)
    const auto result = run_process(
        {"powershell", "-NoProfile", "-Command", "Write-Output (Get-Location).Path"}, dir);
#else
    const auto result = run_process({"pwd"}, dir);
#endif
    EXPECT_EQ(result.exit_code, 0) << "output was: " << result.output;
    // weakly_canonical on both sides: temp paths routinely differ by symlink
    // (macOS /tmp) or 8.3 short names (Windows).
    const auto reported = result.output.substr(0, result.output.find_first_of("\r\n"));
    EXPECT_EQ(std::filesystem::weakly_canonical(reported),
              std::filesystem::weakly_canonical(dir))
        << result.output;
}

TEST(Process, RunProcessFeedsStdin) {
    // Mirrors the real stdin consumer shape (an exe + argument, like
    // `docker-credential-<helper> get`).
#if defined(_WIN32)
    // findstr echoes matching stdin lines; "ping-pong" matches "ping".
    const auto result =
        run_process({"findstr", "ping"}, std::nullopt, {}, std::string("ping-pong\r\n"));
#else
    const auto result = run_process({"cat"}, std::nullopt, {}, std::string("ping-pong\n"));
#endif
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.output.find("ping-pong"), std::string::npos) << result.output;
}
