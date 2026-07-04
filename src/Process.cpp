#include "Process.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "RandomHex.hpp"
#include "testcontainers/Error.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#else
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace testcontainers::detail {

/// See Process.hpp. The one non-obvious rule (and the historical bug source in
/// hand-rolled quoting): a backslash is literal EXCEPT in a run that touches a
/// double quote, where the parser halves it — so runs before an embedded `"`
/// and the trailing run (which touches our closing quote) must be doubled.
std::string quote_arg(const std::string& arg) {
    std::string out;
    out.reserve(arg.size() + 2);
    out.push_back('"');
    std::size_t backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\'); // double the run, escape the quote
        } else {
            out.append(backslashes, '\\'); // plain run: literal as-is
        }
        out.push_back(c);
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\'); // trailing run touches the closing quote
    out.push_back('"');
    return out;
}

std::string build_command_line(const std::vector<std::string>& argv) {
    std::string cmd;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) {
            cmd.push_back(' ');
        }
        cmd += quote_arg(argv[i]);
    }
    return cmd;
}

namespace {

/// Stage stdin_data in a temp file (returned path), or nullopt when unset.
/// A file as the child's stdin cannot deadlock: with a stdin PIPE, a child
/// that fills the output pipe before draining stdin blocks against our write.
std::optional<std::string> stage_stdin_file(const std::optional<std::string>& stdin_data) {
    if (!stdin_data.has_value()) {
        return std::nullopt;
    }
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("tc-stdin-" + random_hex(16) + ".tmp");
    std::ofstream out(path, std::ios::binary);
    out.write(stdin_data->data(), static_cast<std::streamsize>(stdin_data->size()));
    out.close();
    return path.string();
}

void remove_quietly(const std::optional<std::string>& file) {
    if (file.has_value()) {
        std::error_code ec;
        std::filesystem::remove(*file, ec);
    }
}

#if defined(_WIN32)

/// Widen a narrow string with the active code page — the same interpretation
/// the old _popen path (and std::filesystem::path::string()) used, so paths
/// that worked before keep working.
std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = ::MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()),
                                        nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

/// The child's environment block: the parent's variables with `env` overlaid
/// (replace by case-insensitive name, else append), re-sorted case-
/// insensitively as the CreateProcess docs require, double-NUL terminated.
std::wstring build_environment_block(
    const std::vector<std::pair<std::string, std::string>>& env) {
    std::vector<std::wstring> entries;
    {
        LPWCH raw = ::GetEnvironmentStringsW();
        if (raw == nullptr) {
            throw DockerError("run_process: GetEnvironmentStringsW failed");
        }
        for (LPWCH p = raw; *p != L'\0'; p += ::wcslen(p) + 1) {
            entries.emplace_back(p);
        }
        ::FreeEnvironmentStringsW(raw);
    }
    for (const auto& [key, value] : env) {
        const std::wstring name = widen(key);
        const std::wstring entry = name + L"=" + widen(value);
        bool replaced = false;
        for (std::wstring& existing : entries) {
            // Find the separator from position 1: hidden per-drive entries
            // ("=C:=C:\...") legitimately START with '='.
            const std::size_t eq = existing.find(L'=', 1);
            if (eq == name.size() &&
                ::_wcsnicmp(existing.c_str(), name.c_str(), eq) == 0) {
                existing = entry;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            entries.push_back(entry);
        }
    }
    // Whole-entry compare sorts by name ('=' < any name character), which is
    // all the required name ordering needs.
    std::sort(entries.begin(), entries.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return ::_wcsicmp(a.c_str(), b.c_str()) < 0;
              });
    std::wstring block;
    for (const std::wstring& entry : entries) {
        block += entry;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

/// Minimal HANDLE guard so the many early-throw paths below can't leak.
struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~HandleGuard() { reset(); }
    void reset(HANDLE next = INVALID_HANDLE_VALUE) {
        if (h != INVALID_HANDLE_VALUE && h != nullptr) {
            ::CloseHandle(h);
        }
        h = next;
    }
    HandleGuard() = default;
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
};

ProcessResult spawn_and_capture(const std::vector<std::string>& argv,
                                const std::optional<std::string>& working_dir,
                                const std::vector<std::pair<std::string, std::string>>& env,
                                const std::optional<std::string>& stdin_file) {
    std::wstring command_line = widen(build_command_line(argv));
    const std::wstring env_block = build_environment_block(env);
    const std::wstring cwd = working_dir.has_value() ? widen(*working_dir) : std::wstring();

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    // One pipe carries both stdout and stderr (the old `2>&1`); only the write
    // end may be inherited, or the child's copy of the read end would keep the
    // pipe open past its exit and our read loop would never see EOF.
    HandleGuard out_read;
    HandleGuard out_write;
    if (::CreatePipe(&out_read.h, &out_write.h, &inheritable, 0) == 0) {
        throw DockerError("run_process: CreatePipe failed");
    }
    ::SetHandleInformation(out_read.h, HANDLE_FLAG_INHERIT, 0);

    // stdin: the staged temp file, or NUL so the child reads EOF instead of
    // sharing our console input.
    HandleGuard in;
    const std::wstring stdin_path = stdin_file.has_value() ? widen(*stdin_file) : L"NUL";
    in.reset(::CreateFileW(stdin_path.c_str(), GENERIC_READ, FILE_SHARE_READ, &inheritable,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (in.h == INVALID_HANDLE_VALUE) {
        throw DockerError("run_process: failed to open child stdin ('" +
                          (stdin_file.has_value() ? *stdin_file : std::string("NUL")) + "')");
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in.h;
    si.hStdOutput = out_write.h;
    si.hStdError = out_write.h;

    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(
        /*application*/ nullptr, command_line.data(), nullptr, nullptr,
        /*inherit handles*/ TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        const_cast<wchar_t*>(env_block.c_str()), cwd.empty() ? nullptr : cwd.c_str(), &si,
        &pi);
    const DWORD create_error = ::GetLastError();
    // Drop OUR copies of the child-side handles either way; with them open the
    // pipe would never report EOF.
    out_write.reset();
    in.reset();
    if (ok == 0) {
        throw DockerError("run_process: CreateProcessW failed for '" +
                          build_command_line(argv) + "' (error " +
                          std::to_string(create_error) + ")");
    }
    HandleGuard process;
    process.reset(pi.hProcess);
    ::CloseHandle(pi.hThread);

    ProcessResult result;
    char buf[4096];
    DWORD n = 0;
    while (::ReadFile(out_read.h, buf, sizeof(buf), &n, nullptr) != 0 && n > 0) {
        result.output.append(buf, n);
    }

    ::WaitForSingleObject(process.h, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(process.h, &code);
    result.exit_code = static_cast<int>(code);
    return result;
}

#else // POSIX

ProcessResult spawn_and_capture(const std::vector<std::string>& argv,
                                const std::optional<std::string>& working_dir,
                                const std::vector<std::pair<std::string, std::string>>& env,
                                const std::optional<std::string>& stdin_file) {
    // The child's environment: a copy of ours with `env` overlaid.
    std::vector<std::string> merged;
    for (char** p = environ; *p != nullptr; ++p) {
        merged.emplace_back(*p);
    }
    for (const auto& [key, value] : env) {
        const std::string prefix = key + "=";
        const std::string entry = prefix + value;
        bool replaced = false;
        for (std::string& existing : merged) {
            if (existing.compare(0, prefix.size(), prefix) == 0) {
                existing = entry;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            merged.push_back(entry);
        }
    }
    std::vector<char*> envp;
    envp.reserve(merged.size() + 1);
    for (std::string& entry : merged) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
        cargv.push_back(const_cast<char*>(arg.c_str()));
    }
    cargv.push_back(nullptr);

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        throw DockerError("run_process: pipe() failed");
    }
    // Keep our ends out of OTHER children spawned concurrently; the file
    // actions below re-open 1/2 for this child before its exec.
    ::fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_addopen(
        &actions, 0, stdin_file.has_value() ? stdin_file->c_str() : "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_adddup2(&actions, pipefd[1], 1); // stdout
    ::posix_spawn_file_actions_adddup2(&actions, pipefd[1], 2); // stderr (the old 2>&1)
    if (working_dir.has_value()) {
#if (defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))) || \
    defined(__APPLE__)
        ::posix_spawn_file_actions_addchdir_np(&actions, working_dir->c_str());
#else
        ::posix_spawn_file_actions_destroy(&actions);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        throw DockerError(
            "run_process: working_dir requires posix_spawn addchdir support on this libc");
#endif
    }

    pid_t pid = -1;
    const int rc =
        ::posix_spawnp(&pid, argv.empty() ? "" : argv[0].c_str(), &actions, nullptr,
                       cargv.data(), envp.data());
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pipefd[1]);
    if (rc != 0) {
        ::close(pipefd[0]);
        throw DockerError("run_process: posix_spawnp failed for '" +
                          (argv.empty() ? std::string() : argv[0]) + "': " +
                          std::string(::strerror(rc)));
    }

    ProcessResult result;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        if (n > 0) {
            result.output.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    ::close(pipefd[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

#endif

} // namespace

ProcessResult run_process(const std::vector<std::string>& argv,
                          const std::optional<std::string>& working_dir,
                          const std::vector<std::pair<std::string, std::string>>& env,
                          const std::optional<std::string>& stdin_data) {
    if (argv.empty()) {
        throw DockerError("run_process: empty argv");
    }
    const std::optional<std::string> stdin_file = stage_stdin_file(stdin_data);
    try {
        ProcessResult result = spawn_and_capture(argv, working_dir, env, stdin_file);
        remove_quietly(stdin_file);
        return result;
    } catch (...) {
        remove_quietly(stdin_file);
        throw;
    }
}

} // namespace testcontainers::detail
