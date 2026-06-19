#include "Process.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "testcontainers/Error.hpp"

#if defined(_WIN32)
#include <cstdlib> // _putenv_s, _dupenv_s
#define TC_POPEN _popen
#define TC_PCLOSE _pclose
#else
#include <cstdlib> // setenv, unsetenv, getenv
#include <sys/wait.h>
#define TC_POPEN popen
#define TC_PCLOSE pclose
#endif

namespace testcontainers::detail {

namespace {

/// Quote a single argv element for inclusion in a shell command line.
///
/// We wrap every element in double quotes and escape any embedded double quote
/// (`"` -> `\"`). This keeps paths/values containing spaces intact. On Windows
/// the command runs via `cmd /c` (what `_popen` uses); double-quote wrapping is
/// the portable common denominator that both `cmd` and `/bin/sh` accept for our
/// inputs (compose flags, absolute file paths, project names). We do NOT attempt
/// full POSIX/cmd metacharacter escaping — the argv here is library-controlled
/// (compose subcommands + paths), not arbitrary user shell input.
std::string quote_arg(const std::string& arg) {
    std::string out;
    out.reserve(arg.size() + 2);
    out.push_back('"');
    for (const char c : arg) {
        if (c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

/// Build the full command line: each argv element quoted and space-joined, an
/// optional `cd "<dir>" &&` prefix, an optional `< "<stdin-file>"` redirection,
/// and a trailing `2>&1` to capture stderr.
///
/// On Windows, `_popen` runs `cmd.exe /c <line>`. cmd has a notorious rule: when
/// the line begins with a quote, it strips the FIRST and LAST quote of the whole
/// line — which would corrupt our leading `"docker"`. The documented workaround
/// (`cmd /?`) is to wrap the ENTIRE line in one more pair of quotes, so cmd's
/// strip removes that outer pair and leaves our real, per-arg quoting intact.
std::string build_command_line(const std::vector<std::string>& argv,
                               const std::optional<std::string>& working_dir,
                               const std::optional<std::string>& stdin_file) {
    std::string cmd;
    if (working_dir.has_value()) {
        cmd += "cd ";
        cmd += quote_arg(*working_dir);
        cmd += " && ";
    }
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) {
            cmd.push_back(' ');
        }
        cmd += quote_arg(argv[i]);
    }
    if (stdin_file.has_value()) {
        // Redirect stdin from the temp file. cmd/sh both accept `< "path"`; this
        // must come before `2>&1` and is a plain redirection (not an argv token).
        cmd += " < ";
        cmd += quote_arg(*stdin_file);
    }
    cmd += " 2>&1";
#if defined(_WIN32)
    // Wrap the whole line so cmd's first/last-quote stripping is a no-op on it.
    cmd = "\"" + cmd + "\"";
#endif
    return cmd;
}

/// Generate a random lowercase-hex id, mirroring the random_device + hex idiom
/// used elsewhere in the repo (Reaper.cpp, Network.cpp).
std::string random_hex(std::size_t chars) {
    static constexpr char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 15);
    std::string out;
    out.reserve(chars);
    for (std::size_t i = 0; i < chars; ++i) {
        out.push_back(hex[dist(rd)]);
    }
    return out;
}

/// Read an environment variable, returning nullopt when unset. (On Windows
/// `getenv` is flagged unsafe under the secure-CRT; use `_dupenv_s`.)
std::optional<std::string> get_env(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

/// Set / unset an environment variable in the current process.
void set_env(const std::string& name, const std::optional<std::string>& value) {
#if defined(_WIN32)
    // _putenv_s with an empty value REMOVES the variable on Windows, so to set
    // an intentionally-empty value we can't round-trip — acceptable for our use
    // (compose env values are non-empty), and unsetting is what we want anyway.
    _putenv_s(name.c_str(), value.has_value() ? value->c_str() : "");
#else
    if (value.has_value()) {
        setenv(name.c_str(), value->c_str(), /*overwrite*/ 1);
    } else {
        unsetenv(name.c_str());
    }
#endif
}

} // namespace

ProcessResult run_process(const std::vector<std::string>& argv,
                          const std::optional<std::string>& working_dir,
                          const std::vector<std::pair<std::string, std::string>>& env,
                          const std::optional<std::string>& stdin_data) {
    // When stdin is requested, stage it in a temp file we redirect from (popen is
    // unidirectional, so we cannot write to the child's stdin over the pipe).
    std::optional<std::string> stdin_file;
    if (stdin_data.has_value()) {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / ("tc-stdin-" + random_hex(16) + ".tmp");
        std::ofstream out(path, std::ios::binary);
        out.write(stdin_data->data(), static_cast<std::streamsize>(stdin_data->size()));
        out.close();
        stdin_file = path.string();
    }

    // Save prior values, apply the requested env, run, then restore.
    std::vector<std::pair<std::string, std::optional<std::string>>> saved;
    saved.reserve(env.size());
    for (const auto& [key, value] : env) {
        saved.emplace_back(key, get_env(key.c_str()));
        set_env(key, value);
    }

    const std::string command = build_command_line(argv, working_dir, stdin_file);

    ProcessResult result;
    std::string output;
    FILE* pipe = TC_POPEN(command.c_str(), "r");
    if (pipe == nullptr) {
        // Restore env and drop the temp file before throwing.
        for (const auto& [key, prior] : saved) {
            set_env(key, prior);
        }
        if (stdin_file.has_value()) {
            std::error_code ec;
            std::filesystem::remove(*stdin_file, ec);
        }
        throw DockerError("run_process: failed to start '" + command + "'");
    }

    std::array<char, 4096> buf{};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) {
        output.append(buf.data(), n);
    }

    const int status = TC_PCLOSE(pipe);

    // Restore the parent environment.
    for (const auto& [key, prior] : saved) {
        set_env(key, prior);
    }

    // Best-effort cleanup of the staged stdin file.
    if (stdin_file.has_value()) {
        std::error_code ec;
        std::filesystem::remove(*stdin_file, ec);
    }

#if defined(_WIN32)
    result.exit_code = status; // _pclose returns the child's exit code directly
#else
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    result.output = std::move(output);
    return result;
}

} // namespace testcontainers::detail
