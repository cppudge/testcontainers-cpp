#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "Process.hpp"

// Tests in this file:
//   Process.QuoteArgWrapsInDoubleQuotes - a plain token and one with spaces are wrapped in double quotes verbatim.
//   Process.QuoteArgEscapesEmbeddedQuotes - an embedded double quote is backslash-escaped inside the wrapping quotes.
//   Process.BuildCommandLineJoinsQuotedArgs - the argv is quoted, space-joined, and suffixed with 2>&1 (plus cmd.exe's outer wrap on Windows).
//   Process.BuildCommandLineWorkingDirPrefix - a working dir emits a leading cd "<dir>" && prefix.
//   Process.BuildCommandLineStdinRedirectBeforeStderrFold - a stdin file emits < "<file>" between the argv and 2>&1.
//   Process.RunProcessCapturesOutput - a real child process's stdout is captured with exit code 0.
//   Process.RunProcessReportsExitCode - a child exiting non-zero reports that exit code.
//   Process.RunProcessAppliesAndRestoresEnv - env vars are visible to the child and restored (unset) in the parent afterwards.
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
    // The MSVCRT-argv convention (\") — see TODO.md for the cmd.exe caveat this
    // test documents rather than endorses; library-controlled inputs never
    // carry quotes today.
    EXPECT_EQ(quote_arg("a\"b"), "\"a\\\"b\"");
}

TEST(Process, BuildCommandLineJoinsQuotedArgs) {
    const std::string line =
        build_command_line({"docker", "compose", "version"}, std::nullopt, std::nullopt);
    const std::string core = "\"docker\" \"compose\" \"version\" 2>&1";
#if defined(_WIN32)
    // cmd.exe strips the first and last quote of a line that begins with one;
    // the whole line is wrapped so that strip is a no-op on the real quoting.
    EXPECT_EQ(line, "\"" + core + "\"");
#else
    EXPECT_EQ(line, core);
#endif
}

TEST(Process, BuildCommandLineWorkingDirPrefix) {
    const std::string line = build_command_line({"docker"}, "/some dir", std::nullopt);
    EXPECT_NE(line.find("cd \"/some dir\" && \"docker\""), std::string::npos) << line;
}

TEST(Process, BuildCommandLineStdinRedirectBeforeStderrFold) {
    const std::string line = build_command_line({"cat"}, std::nullopt, "/tmp/stdin.tmp");
    const std::size_t redirect = line.find(" < \"/tmp/stdin.tmp\"");
    const std::size_t fold = line.find(" 2>&1");
    ASSERT_NE(redirect, std::string::npos) << line;
    ASSERT_NE(fold, std::string::npos) << line;
    EXPECT_LT(redirect, fold) << "stdin redirection must precede the stderr fold: " << line;
}

// The end-to-end tests below run a real executable as the FIRST argv element
// ("exe + arguments" — the shape every real caller uses: docker, compose,
// docker-credential-<helper>). A nested `cmd /c "<script>"` argv does NOT
// survive run_process's quoting on Windows (cmd re-applies its quote-stripping
// to the inner quoted script and mangles it) — that shape is unsupported and
// deliberately not used here. PowerShell ships with every supported Windows;
// sh/echo/cat are POSIX-guaranteed.

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

TEST(Process, RunProcessReportsExitCode) {
#if defined(_WIN32)
    const auto result = run_process({"powershell", "-NoProfile", "-Command", "exit 3"});
#else
    const auto result = run_process({"sh", "-c", "exit 3"});
#endif
    EXPECT_EQ(result.exit_code, 3);
}

TEST(Process, RunProcessAppliesAndRestoresEnv) {
    ASSERT_EQ(std::getenv("TC_PROC_TEST_ENV"), nullptr)
        << "leftover env from a previous run; unset TC_PROC_TEST_ENV";

    // A real executable (not a nested `cmd /c` script): .exe argv parsing
    // follows the MSVCRT rules our quoting targets, whereas nested cmd mangles
    // a quoted script once %VAR% expansion is involved. PowerShell ships with
    // every supported Windows; sh is POSIX-guaranteed.
#if defined(_WIN32)
    const auto result = run_process(
        {"powershell", "-NoProfile", "-Command", "Write-Output $env:TC_PROC_TEST_ENV"},
        /*working_dir*/ std::nullopt, {{"TC_PROC_TEST_ENV", "visible-in-child"}});
#else
    const auto result = run_process({"sh", "-c", "echo $TC_PROC_TEST_ENV"},
                                    /*working_dir*/ std::nullopt,
                                    {{"TC_PROC_TEST_ENV", "visible-in-child"}});
#endif
    EXPECT_EQ(result.exit_code, 0) << "output was: " << result.output;
    EXPECT_NE(result.output.find("visible-in-child"), std::string::npos) << result.output;
    // The parent's environment is restored (the var was previously unset).
    EXPECT_EQ(std::getenv("TC_PROC_TEST_ENV"), nullptr);
}

TEST(Process, RunProcessFeedsStdin) {
    // Mirrors the real stdin consumer shape (an exe + argument, like
    // `docker-credential-<helper> get`) rather than a nested `cmd /c` script —
    // cmd-inside-cmd mangles the quoted script once a redirection follows it.
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
