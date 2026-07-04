#define _CRT_SECURE_NO_WARNINGS // _putenv_s / std::getenv on MSVC
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
//   Process.RunProcessInheritsParentEnvAndOverlays - the child gets the parent's variables as the base, with overrides REPLACING (not shadow-appending) an existing parent value.
//   Process.RunProcessConcurrentEnvStaysIsolated - two concurrent runs overriding the SAME variable each see their own value (the explicit-env-block guarantee).
//   Process.RunProcessDoesNotLeakUnrelatedHandlesIntoChildren - (Windows) a child inherits ONLY its own std handles, so an unrelated pipe's EOF is not held hostage for the child's lifetime (the concurrent-run_process hazard in miniature).
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

/// Set / unset a variable in THIS process (the parent side of the env tests).
void set_host_env(const char* key, const char* value) {
#if defined(_WIN32)
    _putenv_s(key, value ? value : ""); // empty value removes it
#else
    if (value) {
        setenv(key, value, 1);
    } else {
        unsetenv(key);
    }
#endif
}

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

TEST(Process, RunProcessInheritsParentEnvAndOverlays) {
    // The child's environment block is the PARENT's variables with the
    // overrides overlaid — this is what carries PATH / DOCKER_HOST to
    // docker/compose. Pin both halves: an untouched parent variable is
    // inherited, and an override REPLACES an existing parent value (rather
    // than appending a duplicate entry the child may or may not pick).
    ASSERT_EQ(std::getenv("TC_PROC_BASE_ENV"), nullptr);
    ASSERT_EQ(std::getenv("TC_PROC_OVR_ENV"), nullptr);
    set_host_env("TC_PROC_BASE_ENV", "from-parent");
    set_host_env("TC_PROC_OVR_ENV", "parent-value");

#if defined(_WIN32)
    const auto result = run_process(
        {"powershell", "-NoProfile", "-Command",
         "Write-Output ($env:TC_PROC_BASE_ENV + '|' + $env:TC_PROC_OVR_ENV)"},
        std::nullopt, {{"TC_PROC_OVR_ENV", "child-value"}});
#else
    const auto result =
        run_process({"sh", "-c", "printf '%s|%s' \"$TC_PROC_BASE_ENV\" \"$TC_PROC_OVR_ENV\""},
                    std::nullopt, {{"TC_PROC_OVR_ENV", "child-value"}});
#endif
    EXPECT_EQ(result.exit_code, 0) << "output was: " << result.output;
    EXPECT_NE(result.output.find("from-parent|child-value"), std::string::npos)
        << result.output;
    // The parent's own value survived the run.
    const char* parent_value = std::getenv("TC_PROC_OVR_ENV");
    ASSERT_NE(parent_value, nullptr);
    EXPECT_STREQ(parent_value, "parent-value");

    set_host_env("TC_PROC_BASE_ENV", nullptr);
    set_host_env("TC_PROC_OVR_ENV", nullptr);
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

#if defined(_WIN32)
TEST(Process, RunProcessDoesNotLeakUnrelatedHandlesIntoChildren) {
    // An inheritable pipe UNRELATED to the spawn stands in for a CONCURRENT
    // run_process's pipe write end: with plain bInheritHandles=TRUE the child
    // would inherit a copy and hold the pipe's EOF hostage for its whole
    // lifetime (a short credential-helper run blocking on a long compose up).
    // PROC_THREAD_ATTRIBUTE_HANDLE_LIST must keep it out of the child.
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    ASSERT_NE(::CreatePipe(&read_end, &write_end, &inheritable, 0), 0);

    // A child that signals it is running (flag file) and then outlives the
    // EOF probe below by a wide margin.
    const std::string flag =
        (std::filesystem::temp_directory_path() /
         ("tc-proc-flag-" + std::to_string(::GetCurrentProcessId()) + ".tmp"))
            .string();
    std::filesystem::remove(flag);
    std::thread runner([&flag] {
        run_process({"powershell", "-NoProfile", "-Command",
                     "New-Item -ItemType File -Path '" + flag +
                         "' | Out-Null; Start-Sleep -Seconds 3"});
    });
    const auto spawn_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!std::filesystem::exists(flag)) {
        ASSERT_LT(std::chrono::steady_clock::now(), spawn_deadline)
            << "the flag-file child never started";
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    // Drop OUR only write handle. If the child holds no inherited copy, the
    // read sees EOF immediately; a copy in the child delays it until the
    // child exits (~3s) — the cross-run hang in miniature.
    ::CloseHandle(write_end);
    const auto start = std::chrono::steady_clock::now();
    char buf[8];
    DWORD n = 0;
    const BOOL got_data = ::ReadFile(read_end, buf, sizeof(buf), &n, nullptr);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ::CloseHandle(read_end);
    runner.join();
    std::filesystem::remove(flag);

    EXPECT_TRUE(got_data == 0 && n == 0) << "expected EOF, got data";
    EXPECT_LT(elapsed, std::chrono::milliseconds(1500))
        << "the child inherited an unrelated handle and held EOF hostage";
}
#endif

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
