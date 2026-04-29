// NAAb Persistent Process Executor Implementation
// Manages long-lived interpreter processes via stdin/stdout pipes with sentinel protocol

#include "naab/persistent_process_executor.h"
#include "naab/interpreter.h"  // For Value definition
#include "naab/sandbox.h"      // For security sandbox
#include "naab/resource_limits.h"
#include <cstring>       // For strerror
#include <cerrno>        // For errno
#include <climits>       // For INT_MIN, INT_MAX
#include <stdexcept>
#include <fmt/core.h>
#ifndef _WIN32
#include <csignal>       // For kill, SIGTERM, SIGKILL
#include <fcntl.h>       // For fcntl, F_SETFL, O_NONBLOCK
#include <poll.h>        // For poll
#include <sys/wait.h>    // For waitpid
#endif

namespace naab {
namespace runtime {

PersistentProcessExecutor::PersistentProcessExecutor(
    std::string language_id,
    std::string command,
    std::vector<std::string> args)
    : language_id_(std::move(language_id)),
      command_(std::move(command)),
      args_(std::move(args)) {
}

PersistentProcessExecutor::~PersistentProcessExecutor() {
    // NOTE: Do NOT call stop() here — it calls getExitCommand() which is
    // pure virtual. By the time the base destructor runs, the derived class
    // is already destroyed. Derived classes must call stop() in their own
    // destructors while their vtable is still intact.
#ifndef _WIN32
    // Best-effort cleanup: close fds and kill process without virtual calls
    if (stdin_pipe_fd_ >= 0) { close(stdin_pipe_fd_); stdin_pipe_fd_ = -1; }
    if (stdout_pipe_fd_ >= 0) { close(stdout_pipe_fd_); stdout_pipe_fd_ = -1; }
    if (stderr_pipe_fd_ >= 0) { close(stderr_pipe_fd_); stderr_pipe_fd_ = -1; }
    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        usleep(50000);
        waitpid(child_pid_, nullptr, WNOHANG);
    }
#endif
}

bool PersistentProcessExecutor::start() {
#ifdef _WIN32
    throw std::runtime_error(fmt::format(
        "Persistent {} executor: fork/pipe not supported on Windows", language_id_));
#else
    if (started_ && isAlive()) {
        return true;
    }

    // Create 3 pipes: stdin, stdout, stderr
    int stdin_pipe[2];   // [0]=read (child), [1]=write (parent)
    int stdout_pipe[2];  // [0]=read (parent), [1]=write (child)
    int stderr_pipe[2];  // [0]=read (parent), [1]=write (child)

    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        throw std::runtime_error(fmt::format(
            "Persistent {} executor: Failed to create pipes: {}",
            language_id_, strerror(errno)));
    }

    pid_t pid = fork();
    if (pid < 0) {
        // Fork failed — close all pipe fds
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        throw std::runtime_error(fmt::format(
            "Persistent {} executor: fork() failed: {}",
            language_id_, strerror(errno)));
    }

    if (pid == 0) {
        // === Child process ===
        // V-SC-006: Scrub NAAb internal secrets from child environment
        unsetenv("NAAB_GOVERN_KEY");
        unsetenv("NAAB_LOCK_KEY");
        unsetenv("NAAB_SIGNING_KEY");

        // Close unused pipe ends
        close(stdin_pipe[1]);   // Parent's write end
        close(stdout_pipe[0]);  // Parent's read end
        close(stderr_pipe[0]);  // Parent's read end

        // Redirect stdin/stdout/stderr to pipes
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);

        // Close original fds after dup2
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        // Build argv for execvp
        // argv[0] = command, argv[1..n] = args_, argv[n+1] = nullptr
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(command_.c_str()));
        for (const auto& arg : args_) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(command_.c_str(), argv.data());

        // If we get here, exec failed
        // Write error to stderr (which parent can read)
        std::string err = fmt::format("exec failed: {}: {}\n", command_, strerror(errno));
        (void)write(STDERR_FILENO, err.c_str(), err.size());
        _exit(127);
    }

    // === Parent process ===
    // Close unused pipe ends
    close(stdin_pipe[0]);   // Child's read end
    close(stdout_pipe[1]);  // Child's write end
    close(stderr_pipe[1]);  // Child's write end

    child_pid_ = pid;
    stdin_pipe_fd_ = stdin_pipe[1];
    stdout_pipe_fd_ = stdout_pipe[0];
    stderr_pipe_fd_ = stderr_pipe[0];

    // Set stdout and stderr to non-blocking for polling
    setNonBlocking(stdout_pipe_fd_);
    setNonBlocking(stderr_pipe_fd_);

    started_ = true;

    // Send startup code and consume initial output
    std::string startup = getStartupCode();
    if (!startup.empty()) {
        if (!writeToChild(startup)) {
            stop();
            throw std::runtime_error(fmt::format(
                "Persistent {} executor: Failed to send startup code (process died immediately)\n\n"
                "  Make sure '{}' is installed and in your PATH.\n",
                language_id_, command_));
        }
        // Read until sentinel to confirm startup succeeded
        std::string startup_stderr;
        try {
            readUntilSentinel(startup_stderr, 10000); // 10s startup timeout
        } catch (const std::exception& e) {
            std::string err_detail = startup_stderr.empty() ? drainStderr() : startup_stderr;
            stop();
            throw std::runtime_error(fmt::format(
                "Persistent {} executor: Startup failed: {}\n{}\n\n"
                "  Make sure '{}' is installed and in your PATH.\n",
                language_id_, e.what(), err_detail, command_));
        }
    }

    return true;
#endif // !_WIN32
}

void PersistentProcessExecutor::stop() {
#ifdef _WIN32
    started_ = false;
    child_pid_ = -1;
#else
    if (!started_ && child_pid_ <= 0) {
        return;
    }

    // Try graceful exit
    if (stdin_pipe_fd_ >= 0) {
        std::string exit_cmd = getExitCommand();
        if (!exit_cmd.empty()) {
            // Ignore write errors — process might already be dead
            (void)write(stdin_pipe_fd_, exit_cmd.c_str(), exit_cmd.size());
        }
    }

    // Brief grace period
    usleep(50000); // 50ms

    // Check if child exited
    if (child_pid_ > 0) {
        int status;
        int ret = waitpid(child_pid_, &status, WNOHANG);
        if (ret == 0) {
            // Still alive — send SIGTERM
            kill(child_pid_, SIGTERM);
            usleep(100000); // 100ms

            ret = waitpid(child_pid_, &status, WNOHANG);
            if (ret == 0) {
                // Still alive — SIGKILL
                kill(child_pid_, SIGKILL);
                waitpid(child_pid_, &status, 0); // Blocking reap
            }
        }
        // If ret > 0 or ret == -1, child already exited/reaped
    }

    // Close all fds
    if (stdin_pipe_fd_ >= 0) { close(stdin_pipe_fd_); stdin_pipe_fd_ = -1; }
    if (stdout_pipe_fd_ >= 0) { close(stdout_pipe_fd_); stdout_pipe_fd_ = -1; }
    if (stderr_pipe_fd_ >= 0) { close(stderr_pipe_fd_); stderr_pipe_fd_ = -1; }

    child_pid_ = -1;
    started_ = false;
#endif // !_WIN32
}

bool PersistentProcessExecutor::restart() {
    stop();
    return start();
}

bool PersistentProcessExecutor::isAlive() const {
#ifdef _WIN32
    return false;
#else
    if (child_pid_ <= 0) return false;

    int status;
    int ret = waitpid(child_pid_, &status, WNOHANG);
    if (ret == 0) {
        // Child still running
        return true;
    }
    if (ret > 0 || (ret == -1 && errno == ECHILD)) {
        // Child exited or already reaped
        // Can't modify members in const method, but caller will handle
        return false;
    }
    return false;
#endif // !_WIN32
}

bool PersistentProcessExecutor::execute(const std::string& code) {
    try {
        executeWithReturn(code);
        return true;
    } catch (...) {
        return false;
    }
}

interpreter::NaabVal PersistentProcessExecutor::executeWithReturn(
    const std::string& code) {

    std::lock_guard<std::mutex> lock(process_mutex_);

    // Enterprise Security: Install signal handlers for resource limits (once)
    if (!security::ResourceLimiter::isInitialized()) {
        security::ResourceLimiter::installSignalHandlers();
    }

    // Enterprise Security: FAIL-CLOSED - Check if system command execution is allowed
    auto* sandbox = security::ScopedSandbox::getCurrent();
    bool execution_allowed = false;
    if (sandbox) {
        execution_allowed = sandbox->getConfig().allow_exec &&
                           sandbox->getConfig().hasCapability(security::Capability::SYS_EXEC);
    }

    if (!execution_allowed) {
        throw std::runtime_error(fmt::format(
            "Security: {} execution denied by sandbox\n\n"
            "  Polyglot blocks can execute arbitrary code.\n"
            "  For security, execution is disabled by default.\n\n"
            "  The project owner can adjust the sandbox level in the project configuration.\n",
            language_id_));
    }

    // Get timeout from sandbox config
    unsigned int timeout = 30;
    if (sandbox) {
        timeout = sandbox->getConfig().max_cpu_seconds;
    }
    int timeout_ms = static_cast<int>(timeout) * 1000;

    // Lazy start
    if (!started_) {
        start();
    }

    // Auto-restart if process died between blocks
    if (!isAlive()) {
        started_ = false;
        restart();
    }

    // Wrap user code with sentinel protocol
    std::string wrapped = wrapCodeForExecution(code);

    // Send wrapped code to child's stdin
    if (!writeToChild(wrapped)) {
        // Process died while writing — restart and retry once
        started_ = false;
        restart();
        if (!writeToChild(wrapped)) {
            throw std::runtime_error(fmt::format(
                "Persistent {} executor: Process died during code submission",
                language_id_));
        }
    }

    // Read output until sentinel
    std::string stderr_text;
    std::string stdout_text;
    try {
        stdout_text = readUntilSentinel(stderr_text, timeout_ms);
    } catch (const std::runtime_error& e) {
        // Process might have crashed — drain any remaining stderr
        std::string extra_err = drainStderr();
        if (!extra_err.empty()) {
            stderr_text += extra_err;
        }
        // If process died, mark as not started for auto-restart on next call
        if (!isAlive()) {
            started_ = false;
        }
        throw std::runtime_error(fmt::format(
            "{} execution error: {}\n{}",
            language_id_, e.what(), stderr_text));
    }

    execution_count_++;

    // Buffer stderr for getCapturedOutput()
    if (!stderr_text.empty()) {
        stderr_buffer_.append(stderr_text);
    }

    return parseOutput(stdout_text, stderr_text, 0);
}

interpreter::NaabVal PersistentProcessExecutor::callFunction(
    const std::string& function_name,
    const std::vector<interpreter::NaabVal>& args) {

    if (function_name == "exec" && !args.empty()) {
        if (args[0].isString()) {
            bool success = execute(args[0].asString());
            return interpreter::NaabVal::makeBool(success);
        }
        if (args[0].isInt()) {
            bool success = execute(std::to_string(args[0].asInt()));
            return interpreter::NaabVal::makeBool(success);
        }
    }

    throw std::runtime_error(fmt::format(
        "Persistent {} executor only supports 'exec(code_string)'",
        language_id_));
}

std::string PersistentProcessExecutor::getCapturedOutput() {
    std::string output = stdout_buffer_.getAndClear();
    std::string error_output = stderr_buffer_.getAndClear();
    if (!error_output.empty()) {
        output += "\n[" + language_id_ + " stderr]: " + error_output;
    }
    return output;
}

bool PersistentProcessExecutor::isInitialized() const {
    return true; // Initialized on first use (lazy start)
}

std::string PersistentProcessExecutor::getLanguage() const {
    return language_id_;
}

// Default output parsing — same logic as GenericSubprocessExecutor/ShellExecutor
interpreter::NaabVal PersistentProcessExecutor::parseOutput(
    const std::string& stdout_text, const std::string& /* stderr_text */,
    int /* implicit_exit_code */) const {

    // Trim trailing whitespace/newlines
    std::string result = stdout_text;
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' ||
                                result.back() == ' ' || result.back() == '\t')) {
        result.pop_back();
    }

    // Empty result -> null
    if (result.empty()) {
        return interpreter::NaabVal::makeNull();
    }

    // Null representations
    if (result == "null" || result == "NULL" || result == "None" ||
        result == "nil" || result == "Nil" || result == "<nil>" ||
        result == "nothing" || result == "undefined" || result == "()") {
        return interpreter::NaabVal::makeNull();
    }

    // Boolean representations
    if (result == "true" || result == "True" || result == "TRUE") {
        return interpreter::NaabVal::makeBool(true);
    }
    if (result == "false" || result == "False" || result == "FALSE") {
        return interpreter::NaabVal::makeBool(false);
    }

    // Try to parse as number (double first for large numbers)
    try {
        size_t pos;
        double d = std::stod(result, &pos);
        if (pos == result.size()) {
            // Check if it's actually an integer that fits in int range
            if (d == static_cast<int>(d) && d >= INT_MIN && d <= INT_MAX) {
                return interpreter::NaabVal::makeInt(static_cast<int>(d));
            }
            return interpreter::NaabVal::makeDouble(d);
        }
    } catch (...) {}

    // Return as string
    return interpreter::NaabVal::makeString(result);
}

// === Private methods ===

bool PersistentProcessExecutor::writeToChild(const std::string& data) {
#ifdef _WIN32
    (void)data;
    return false;
#else
    if (stdin_pipe_fd_ < 0) return false;

    size_t total_written = 0;
    while (total_written < data.size()) {
        ssize_t written = write(stdin_pipe_fd_, data.c_str() + total_written,
                                data.size() - total_written);
        if (written < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) return false; // Child dead
            return false;
        }
        total_written += static_cast<size_t>(written);
    }
    return true;
#endif // !_WIN32
}

std::string PersistentProcessExecutor::readUntilSentinel(std::string& stderr_text, int timeout_ms) {
#ifdef _WIN32
    (void)stderr_text; (void)timeout_ms;
    throw std::runtime_error("readUntilSentinel not supported on Windows");
#else
    std::string accumulated;
    std::string sentinel = getSentinel();
    std::string sentinel_line = sentinel + "\n";

    auto start_time = std::chrono::steady_clock::now();

    char buf[4096];

    while (true) {
        // Calculate remaining timeout
        auto now = std::chrono::steady_clock::now();
        int elapsed_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count());
        int remaining_ms = timeout_ms - elapsed_ms;

        if (remaining_ms <= 0) {
            // Timeout — kill the process
            if (child_pid_ > 0) {
                kill(child_pid_, SIGKILL);
                int status;
                waitpid(child_pid_, &status, 0);
            }
            started_ = false;
            throw std::runtime_error(fmt::format(
                "Persistent {} executor: Timeout after {}ms waiting for output",
                language_id_, timeout_ms));
        }

        // Poll stdout with remaining timeout
        struct pollfd fds[2];
        fds[0].fd = stdout_pipe_fd_;
        fds[0].events = POLLIN;
        fds[1].fd = stderr_pipe_fd_;
        fds[1].events = POLLIN;

        int poll_ret = poll(fds, 2, std::min(remaining_ms, 100)); // Poll at most 100ms at a time

        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(fmt::format(
                "Persistent {} executor: poll() error: {}",
                language_id_, strerror(errno)));
        }

        // Read stderr (non-blocking, always drain)
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(stderr_pipe_fd_, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                stderr_text += buf;
            }
        }

        // Read stdout
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(stdout_pipe_fd_, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                accumulated += buf;

                // Check if sentinel appears on its own line
                // Look for "\n<sentinel>\n" or accumulated starts with "<sentinel>\n"
                std::string::size_type pos = std::string::npos;

                // Check for sentinel at start of accumulated
                if (accumulated.find(sentinel_line) == 0) {
                    pos = 0;
                } else {
                    // Check for sentinel after a newline
                    std::string needle = "\n" + sentinel_line;
                    pos = accumulated.find(needle);
                    if (pos != std::string::npos) {
                        pos += 1; // Skip the leading \n
                    } else {
                        // Check if accumulated ends with "\n<sentinel>" (no trailing \n yet)
                        std::string end_needle = "\n" + sentinel;
                        if (accumulated.size() >= end_needle.size() &&
                            accumulated.compare(accumulated.size() - end_needle.size(),
                                               end_needle.size(), end_needle) == 0) {
                            pos = accumulated.size() - sentinel.size();
                        }
                        // Or starts with sentinel (no leading \n)
                        if (pos == std::string::npos && accumulated == sentinel) {
                            pos = 0;
                        }
                    }
                }

                if (pos != std::string::npos) {
                    // Found sentinel — return everything before it
                    return accumulated.substr(0, pos);
                }
            } else if (n == 0) {
                // EOF — child closed stdout (process died)
                started_ = false;
                if (!accumulated.empty()) {
                    throw std::runtime_error(fmt::format(
                        "Persistent {} executor: Process exited unexpectedly.\nPartial output: {}",
                        language_id_, accumulated));
                }
                throw std::runtime_error(fmt::format(
                    "Persistent {} executor: Process exited unexpectedly",
                    language_id_));
            }
        }

        // Check for HUP/ERR on stdout (process died)
        if (fds[0].revents & (POLLHUP | POLLERR)) {
            // Try one more read to get remaining data
            ssize_t n = read(stdout_pipe_fd_, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                accumulated += buf;

                // One last sentinel check
                std::string needle = "\n" + sentinel;
                auto pos = accumulated.find(sentinel);
                if (pos != std::string::npos) {
                    // Verify it's on its own line
                    if (pos == 0 || accumulated[pos - 1] == '\n') {
                        return accumulated.substr(0, pos);
                    }
                }
            }

            started_ = false;
            if (!accumulated.empty()) {
                throw std::runtime_error(fmt::format(
                    "Persistent {} executor: Process exited unexpectedly.\nPartial output: {}",
                    language_id_, accumulated));
            }
            throw std::runtime_error(fmt::format(
                "Persistent {} executor: Process exited unexpectedly",
                language_id_));
        }
    }
#endif // !_WIN32
}

std::string PersistentProcessExecutor::drainStderr() {
#ifdef _WIN32
    return "";
#else
    if (stderr_pipe_fd_ < 0) return "";

    std::string result;
    char buf[4096];

    while (true) {
        ssize_t n = read(stderr_pipe_fd_, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            result += buf;
        } else {
            break; // EAGAIN (no data) or error
        }
    }
    return result;
#endif // !_WIN32
}

void PersistentProcessExecutor::setNonBlocking(int fd) {
#ifndef _WIN32
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
#else
    (void)fd;
#endif
}

} // namespace runtime
} // namespace naab
