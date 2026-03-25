#pragma once

// NAAb Persistent Process Executor
// Keeps interpreter processes alive via stdin/stdout pipes with a sentinel protocol.
// Amortizes startup cost to one process per language per program.
// Preserves state between blocks.

#include "naab/language_registry.h"
#include "naab/output_buffer.h"
#include <string>
#include <vector>
#include <mutex>
#include <unistd.h>  // pid_t

namespace naab {
namespace runtime {

class PersistentProcessExecutor : public Executor {
public:
    PersistentProcessExecutor(std::string language_id,
                               std::string command,
                               std::vector<std::string> args);
    ~PersistentProcessExecutor() override;

    // Executor interface
    bool execute(const std::string& code) override;
    interpreter::NaabVal executeWithReturn(const std::string& code) override;
    interpreter::NaabVal callFunction(
        const std::string& function_name,
        const std::vector<interpreter::NaabVal>& args) override;
    std::string getCapturedOutput() override;
    bool isInitialized() const override;
    std::string getLanguage() const override;

    // Lifecycle management
    bool start();           // Spawn child process, open pipes, send startup code
    void stop();            // Graceful shutdown: exit command -> close fds -> waitpid
    bool restart();         // stop() + start()
    bool isAlive() const;   // Check child_pid_ via waitpid(WNOHANG)

protected:
    // === Language-specific hooks (pure virtual) ===

    // Wrap user code so that: (a) it executes, (b) sentinel is printed after.
    // Must handle errors gracefully (try-catch equivalent per language).
    virtual std::string wrapCodeForExecution(const std::string& code) const = 0;

    // The sentinel string that marks end-of-output for one block
    virtual std::string getSentinel() const = 0;

    // Code injected once after process starts (define helpers, disable prompts, etc.)
    virtual std::string getStartupCode() const = 0;

    // Command to cleanly exit the interpreter
    virtual std::string getExitCommand() const = 0;

    // Parse raw stdout captured between two sentinels into a NAAb Value.
    // Default: Same value parsing as GenericSubprocessExecutor (null/bool/int/float/string)
    virtual interpreter::NaabVal parseOutput(
        const std::string& stdout_text, const std::string& stderr_text,
        int implicit_exit_code) const;

private:
    std::string language_id_;
    std::string command_;
    std::vector<std::string> args_;

    // Process state
    pid_t child_pid_ = -1;
    int stdin_pipe_fd_ = -1;   // Parent writes to child's stdin
    int stdout_pipe_fd_ = -1;  // Parent reads child's stdout
    int stderr_pipe_fd_ = -1;  // Parent reads child's stderr (non-blocking)
    bool started_ = false;
    int execution_count_ = 0;  // For diagnostics

    // Thread safety — one block at a time per process
    std::mutex process_mutex_;

    // Output buffers (getCapturedOutput support)
    OutputBuffer stdout_buffer_;
    OutputBuffer stderr_buffer_;

    // === Pipe I/O ===

    // Write data to child's stdin. Returns false on EPIPE (child dead).
    bool writeToChild(const std::string& data);

    // Read from child's stdout until sentinel line appears.
    // Returns all stdout lines BEFORE the sentinel.
    // Timeout: kills process and throws on timeout.
    // Also drains stderr into stderr_text.
    std::string readUntilSentinel(std::string& stderr_text, int timeout_ms);

    // Non-blocking drain of stderr pipe into string
    std::string drainStderr();

    // Set a file descriptor to non-blocking mode
    static void setNonBlocking(int fd);
};

} // namespace runtime
} // namespace naab
