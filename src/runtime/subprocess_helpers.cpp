// NAAb Subprocess Helpers Implementation
// Contains common utility functions for executing subprocesses

#include "naab/subprocess_helpers.h"
#include <cstdio>       // For popen, pclose, FILE
#include <array>        // For std::array
#include <stdexcept>    // For std::runtime_error
#include <fmt/core.h>   // For fmt::print
#include <sstream>      // For std::istringstream

// For fork/execvp/pipe
#include <unistd.h>     // For fork, pipe, dup2, close, _exit, execvp, execvpe
#include <sys/wait.h>   // For waitpid, WIFEXITED, WEXITSTATUS
#include <fcntl.h>      // For file descriptor manipulation
#include <vector>       // For std::vector
#include <map>          // For std::map
#include <cstring>      // For strdup, free

namespace naab {
namespace runtime {

// Helper to execute a subprocess and capture its stdout/stderr separately
// Returns exit code, fills stdout_str and stderr_str
int execute_subprocess_with_pipes(
    const std::string& command_path,
    const std::vector<std::string>& args,
    std::string& stdout_str,
    std::string& stderr_str,
    const std::map<std::string, std::string>* env) {

    int stdout_pipe[2]; // 0 for read, 1 for write
    int stderr_pipe[2]; // 0 for read, 1 for write

    if (pipe(stdout_pipe) == -1 || pipe(stderr_pipe) == -1) {
        perror("pipe");
        return -1; // Indicate error
    }

    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return -1; // Indicate error
    }

    if (pid == 0) { // Child process
        close(stdout_pipe[0]); // Child closes read end of stdout pipe
        close(stderr_pipe[0]); // Child closes read end of stderr pipe

        dup2(stdout_pipe[1], STDOUT_FILENO); // Redirect stdout to pipe
        dup2(stderr_pipe[1], STDERR_FILENO); // Redirect stderr to pipe

        close(stdout_pipe[1]); // Close original write end
        close(stderr_pipe[1]); // Close original write end

        // Prepare arguments for execvp
        std::vector<char*> argv_c_str;
        argv_c_str.push_back(const_cast<char*>(command_path.c_str()));
        for (const auto& arg : args) {
            argv_c_str.push_back(const_cast<char*>(arg.c_str()));
        }
        argv_c_str.push_back(nullptr); // Null-terminate the array

        // Prepare environment for execvp if provided
        // Use std::string to avoid manual memory management
        std::vector<std::string> envp_strings;
        std::vector<char*> envp_c_str_ptrs;
        if (env) {
            for (const auto& pair : *env) {
                envp_strings.push_back(pair.first + "=" + pair.second);
            }
            for (auto& str : envp_strings) {
                envp_c_str_ptrs.push_back(const_cast<char*>(str.c_str()));
            }
            envp_c_str_ptrs.push_back(nullptr); // Null-terminate
        }

        if (env) {
            execvpe(command_path.c_str(), argv_c_str.data(), envp_c_str_ptrs.data());
        } else {
            execvp(command_path.c_str(), argv_c_str.data());
        }

        // If execvp returns, an error occurred
        perror("execvp/execvpe");
        _exit(1); // Exit child process (envp_strings automatically freed)
    } else { // Parent process
        close(stdout_pipe[1]); // Parent closes write end of stdout pipe
        close(stderr_pipe[1]); // Parent closes write end of stderr pipe

        std::array<char, 4096> buffer;
        ssize_t bytes_read;

        // Read from stdout pipe
        while ((bytes_read = read(stdout_pipe[0], buffer.data(), buffer.size())) > 0) {
            stdout_str.append(buffer.data(), bytes_read);
        }
        close(stdout_pipe[0]);

        // Read from stderr pipe
        while ((bytes_read = read(stderr_pipe[0], buffer.data(), buffer.size())) > 0) {
            stderr_str.append(buffer.data(), bytes_read);
        }
        close(stderr_pipe[0]);

        int status;
        waitpid(pid, &status, 0); // Wait for child to exit

        // No cleanup needed - envp_strings are automatically freed when child exits or execs

        if (WIFEXITED(status)) {
            return WEXITSTATUS(status); // Return child's exit status
        } else {
            return -1; // Child terminated abnormally
        }
    }
}

} // namespace runtime
} // namespace naab
