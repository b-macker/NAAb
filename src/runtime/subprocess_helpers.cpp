// NAAb Subprocess Helpers Implementation
// Contains common utility functions for executing subprocesses
//
// Uses fork()/execvp() for subprocess execution. This avoids shell
// interpretation entirely, preventing command injection vulnerabilities.
// Arguments are passed directly to the kernel via execvp's argv array.

#include "naab/subprocess_helpers.h"
#include "naab/paths.h"
#include "naab/resource_limits.h"  // V-RT-003: isTimeoutTriggered() for orphan prevention
#include <cstdio>       // For FILE, fopen, fclose, fprintf
#include <fmt/core.h>   // For fmt::format
#include <sstream>      // For std::ostringstream
#include <fstream>      // For std::ifstream
#include <vector>       // For std::vector
#include <map>          // For std::map
#include <cstring>      // For strerror / strsignal
#include <cstdlib>      // For mkstemp/environ (POSIX) or _putenv_s (Windows)
#include <cerrno>       // For errno

#ifndef _WIN32
#  include <unistd.h>     // For fork, execvp, dup2, unlink, getpid, _exit
#  include <sys/wait.h>   // For waitpid, WIFEXITED, WEXITSTATUS, WIFSIGNALED
#  include <sys/resource.h> // For getrlimit, RLIMIT_AS
#else
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace naab {
namespace runtime {

// Helper to read entire file contents into a string
static std::string readFileContents(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
}

#ifndef _WIN32
// Check if a process-wide memory limit (RLIMIT_AS) is currently active.
// Returns the limit in MB if set, or 0 if unlimited.
static size_t getActiveMemoryLimitMB() {
    struct rlimit limit;
    if (getrlimit(RLIMIT_AS, &limit) == 0) {
        if (limit.rlim_cur != RLIM_INFINITY) {
            return limit.rlim_cur / (1024 * 1024);
        }
    }
    return 0;
}

// Build a helpful error message when subprocess execution fails and a
// process-wide memory limit (RLIMIT_AS) is active. This detects the exact
// scenario that caused hours of debugging: JS executor set RLIMIT_AS=512MB
// via ResourceLimiter::setMemoryLimit() which persisted and broke all
// subsequent fork/exec/system calls (child processes couldn't allocate
// memory for the dynamic linker, causing SIGABRT).
static std::string buildMemoryLimitError(
    const std::string& command,
    int signal_num,
    size_t memory_limit_mb) {

    std::ostringstream oss;
    oss << "Subprocess error: Command failed";
    if (signal_num > 0) {
        oss << " with signal " << signal_num << " (" << strsignal(signal_num) << ")";
    }
    oss << "\n\n";

    oss << "  Command: " << command << "\n\n";

    if (memory_limit_mb > 0) {
        oss << "  Process-wide memory limit detected: RLIMIT_AS = "
            << memory_limit_mb << " MB\n\n";

        oss << "  This is likely the cause of the failure.\n"
            << "  RLIMIT_AS limits the total virtual address space for the entire\n"
            << "  process AND all child processes created via fork/exec/system.\n"
            << "  When set too low, child processes cannot allocate memory for the\n"
            << "  dynamic linker, causing SIGABRT or SIGSEGV on startup.\n\n";

        oss << "  Common causes:\n"
            << "  - A polyglot executor called ResourceLimiter::setMemoryLimit()\n"
            << "    which sets RLIMIT_AS process-wide and never clears it\n"
            << "  - Sandbox configuration set max_memory_mb too low\n"
            << "  - An external tool or wrapper set RLIMIT_AS before launch\n\n";

        oss << "  How to fix:\n"
            << "  - Use language-native memory limits instead of RLIMIT_AS\n"
            << "    (e.g., JS_SetMemoryLimit for QuickJS, not setrlimit)\n"
            << "  - Clear the limit after use: ResourceLimiter::disableAll()\n"
            << "  - Check sandbox config: max_memory_mb should be 0 (unlimited)\n"
            << "    for executors that spawn subprocesses\n\n";

        oss << "  Diagnostic:\n"
            << "    Current RLIMIT_AS: " << memory_limit_mb << " MB ("
            << (memory_limit_mb * 1024 * 1024) << " bytes)\n"
            << "    Typical minimum for fork/exec: ~150-300 MB\n"
            << "    Recommendation: Use RLIM_INFINITY or language-native limits\n";
    } else {
        oss << "  No RLIMIT_AS restriction detected.\n"
            << "  The failure may be caused by:\n"
            << "  - Command not found (check PATH)\n"
            << "  - Missing shared libraries\n"
            << "  - Insufficient file descriptors (RLIMIT_NOFILE)\n"
            << "  - Sandbox or seccomp restrictions\n";
    }

    return oss.str();
}
#endif // !_WIN32

// Helper to execute a subprocess and capture its stdout/stderr.
// Returns exit code, fills stdout_str and stderr_str.
//
// On POSIX: uses fork()/execvp() with temp files for output capture.
// On Windows: uses CreateProcessA() with temp files for output capture.
//
// IMPORTANT: If this returns -1 with SIGABRT on POSIX, check stderr for
// memory limit diagnostics. Process-wide RLIMIT_AS is the #1 cause of
// mysterious subprocess failures in polyglot execution.
int execute_subprocess_with_pipes(
    const std::string& command_path,
    const std::vector<std::string>& args,
    std::string& stdout_str,
    std::string& stderr_str,
    const std::map<std::string, std::string>* env) {

#ifdef _WIN32
    // =========================================================================
    // Windows implementation — CreateProcessA + temp files
    // =========================================================================
    std::string tmp = naab::paths::temp_dir();
    char stdout_tmp_buf[MAX_PATH];
    char stderr_tmp_buf[MAX_PATH];
    char tmp_prefix[MAX_PATH];

    // GetTempPathA may return path with or without trailing backslash
    std::string tmp_win = tmp;
    if (!tmp_win.empty() && tmp_win.back() != '\\' && tmp_win.back() != '/') {
        tmp_win += '\\';
    }
    strncpy(tmp_prefix, tmp_win.c_str(), MAX_PATH - 1);
    tmp_prefix[MAX_PATH - 1] = '\0';

    if (!::GetTempFileNameA(tmp_prefix, "nso", 0, stdout_tmp_buf) ||
        !::GetTempFileNameA(tmp_prefix, "nse", 0, stderr_tmp_buf)) {
        fprintf(stderr, "[subprocess] GetTempFileName failed (error %lu)\n", ::GetLastError());
        return -1;
    }
    std::string stdout_tmp(stdout_tmp_buf);
    std::string stderr_tmp(stderr_tmp_buf);

    // Build flat command-line string: "program" "arg1" "arg2" ...
    // Args containing spaces or quotes are double-quoted; internal quotes escaped.
    auto quoteArg = [](const std::string& s) -> std::string {
        bool needs_quote = s.empty() ||
                           s.find(' ') != std::string::npos ||
                           s.find('"') != std::string::npos ||
                           s.find('\t') != std::string::npos;
        if (!needs_quote) return s;
        std::string result = "\"";
        for (char c : s) {
            if (c == '"') result += "\\\"";
            else result += c;
        }
        result += '"';
        return result;
    };

    std::string cmdline = quoteArg(command_path);
    for (const auto& arg : args) {
        cmdline += ' ';
        cmdline += quoteArg(arg);
    }

    // Build custom environment block if requested (double-null-terminated)
    std::vector<char> env_block;
    LPVOID env_ptr = nullptr;
    if (env && !env->empty()) {
        // Start with current process environment
        LPCH cur_env = ::GetEnvironmentStringsA();
        if (cur_env) {
            for (LPCH p = cur_env; *p; ) {
                std::string entry(p);
                p += entry.size() + 1;
                // Skip keys that we're overriding
                size_t eq = entry.find('=');
                if (eq != std::string::npos) {
                    std::string key = entry.substr(0, eq);
                    if (env->count(key) == 0) {
                        for (char c : entry) env_block.push_back(c);
                        env_block.push_back('\0');
                    }
                }
            }
            ::FreeEnvironmentStringsA(cur_env);
        }
        // Add custom vars
        for (const auto& pair : *env) {
            std::string entry = pair.first + "=" + pair.second;
            for (char c : entry) env_block.push_back(c);
            env_block.push_back('\0');
        }
        env_block.push_back('\0');  // Double-null terminator
        env_ptr = env_block.data();
    }

    // Open temp files as inheritable handles for stdout/stderr
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hOut = ::CreateFileA(stdout_tmp.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE hErr = ::CreateFileA(stderr_tmp.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE) {
        if (hOut != INVALID_HANDLE_VALUE) ::CloseHandle(hOut);
        if (hErr != INVALID_HANDLE_VALUE) ::CloseHandle(hErr);
        fprintf(stderr, "[subprocess] CreateFile for temp output failed (error %lu)\n",
                ::GetLastError());
        ::DeleteFileA(stdout_tmp.c_str());
        ::DeleteFileA(stderr_tmp.c_str());
        return -1;
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hOut;
    si.hStdError = hErr;

    PROCESS_INFORMATION pi = {};
    std::vector<char> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back('\0');

    BOOL ok = ::CreateProcessA(
        nullptr,
        cmdline_buf.data(),
        nullptr, nullptr,
        TRUE,   // inherit handles
        0,
        env_ptr,
        nullptr,
        &si, &pi);

    ::CloseHandle(hOut);
    ::CloseHandle(hErr);

    if (!ok) {
        DWORD err = ::GetLastError();
        fprintf(stderr, "[subprocess] CreateProcess failed for '%s' (error %lu)\n",
                command_path.c_str(), err);
        ::DeleteFileA(stdout_tmp.c_str());
        ::DeleteFileA(stderr_tmp.c_str());
        return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? 127 : -1;
    }

    ::WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 1;
    ::GetExitCodeProcess(pi.hProcess, &exit_code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);

    stdout_str = readFileContents(stdout_tmp);
    stderr_str = readFileContents(stderr_tmp);

    ::DeleteFileA(stdout_tmp.c_str());
    ::DeleteFileA(stderr_tmp.c_str());

    return static_cast<int>(exit_code);

#else
    // =========================================================================
    // POSIX implementation — fork()/execvp() + temp files
    // =========================================================================

    // Create temp files for stdout and stderr capture
    std::string tmp = naab::paths::temp_dir();
    std::string stdout_tmp = tmp + "/naab_out_XXXXXX";
    std::string stderr_tmp = tmp + "/naab_err_XXXXXX";

    int stdout_fd = mkstemp(&stdout_tmp[0]);
    int stderr_fd = mkstemp(&stderr_tmp[0]);

    if (stdout_fd == -1 || stderr_fd == -1) {
        // Fallback paths if mkstemp fails
        stdout_tmp = fmt::format("{}/naab_out_{}", tmp, getpid());
        stderr_tmp = fmt::format("{}/naab_err_{}", tmp, getpid());
    } else {
        close(stdout_fd);
        close(stderr_fd);
    }

    // Build argv array for execvp (no shell interpretation)
    std::vector<const char*> argv;
    argv.push_back(command_path.c_str());
    for (const auto& arg : args) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    // Build envp array if custom environment provided
    std::vector<std::string> env_strings;
    std::vector<const char*> envp;
    bool use_custom_env = false;
    if (env && !env->empty()) {
        use_custom_env = true;
        // Inherit current environment
        for (char** e = environ; *e != nullptr; ++e) {
            env_strings.push_back(*e);
        }
        // Add/override with custom vars
        for (const auto& pair : *env) {
            env_strings.push_back(pair.first + "=" + pair.second);
        }
        for (const auto& s : env_strings) {
            envp.push_back(s.c_str());
        }
        envp.push_back(nullptr);
    }

    // Fork and exec (avoids shell interpretation — no command injection possible)
    pid_t pid = fork();
    if (pid == -1) {
        // Fork failed
        unlink(stdout_tmp.c_str());
        unlink(stderr_tmp.c_str());
        size_t mem_limit = getActiveMemoryLimitMB();
        std::string error_msg = buildMemoryLimitError(command_path, 0, mem_limit);
        fprintf(stderr, "%s\n", error_msg.c_str());
        return -1;
    }

    if (pid == 0) {
        // Child process: redirect stdout/stderr to temp files
        FILE* out = fopen(stdout_tmp.c_str(), "w");
        FILE* err = fopen(stderr_tmp.c_str(), "w");
        if (out) { dup2(fileno(out), STDOUT_FILENO); fclose(out); }
        if (err) { dup2(fileno(err), STDERR_FILENO); fclose(err); }

        if (use_custom_env) {
            execve(command_path.c_str(),
                   const_cast<char* const*>(argv.data()),
                   const_cast<char* const*>(envp.data()));
        } else {
            execvp(command_path.c_str(),
                   const_cast<char* const*>(argv.data()));
        }
        // exec failed
        _exit(127);
    }

    // Parent process: wait for child.
    // V-RT-003: use a poll loop instead of blocking waitpid(). If the execution
    // timeout fires while we're blocked here (SA_RESTART means SIGALRM is silently
    // restarted — it does NOT interrupt waitpid), we kill the child before it
    // becomes an orphan. Poll every 50ms to keep CPU overhead negligible.
    int status = 0;
    while (true) {
        pid_t wait_result = waitpid(pid, &status, WNOHANG);
        if (wait_result == pid) break;          // Child exited — done
        if (wait_result == -1) {                // waitpid error (EINTR etc.)
            status = -1;
            break;
        }
        // Child still running — check if timeout fired
        if (naab::security::ResourceLimiter::isTimeoutTriggered()) {
            ::kill(pid, SIGKILL);               // Kill before it becomes an orphan
            waitpid(pid, &status, 0);           // Reap zombie (fast after SIGKILL)
            unlink(stdout_tmp.c_str());
            unlink(stderr_tmp.c_str());
            throw std::runtime_error(
                "Subprocess timeout: child process killed to prevent orphan.\n\n"
                "  The subprocess exceeded the execution time limit and was terminated.\n"
                "  Use --timeout N to set a longer limit.\n"
            );
        }
        usleep(50000);  // 50ms poll interval
    }

    // Read captured output
    stdout_str = readFileContents(stdout_tmp);
    stderr_str = readFileContents(stderr_tmp);

    // Clean up temp files
    unlink(stdout_tmp.c_str());
    unlink(stderr_tmp.c_str());

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);

        // Check if this looks like a memory-limit-induced crash.
        if (sig == SIGABRT || sig == SIGSEGV || sig == SIGKILL) {
            size_t mem_limit = getActiveMemoryLimitMB();
            if (mem_limit > 0) {
                std::string error_msg = buildMemoryLimitError(command_path, sig, mem_limit);
                fprintf(stderr, "%s\n", error_msg.c_str());
                return -1;
            }
        }

        fprintf(stderr, "[subprocess] Child killed by signal %d (%s)\n",
                sig, strsignal(sig));
        return -1;
    }

    return -1;
#endif
}

} // namespace runtime
} // namespace naab
