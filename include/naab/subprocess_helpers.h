#pragma once

#include <string>
#include <vector>
#include <map>
#include <unistd.h>     // For pid_t

namespace naab {
namespace runtime {

// Helper to execute a subprocess and capture its stdout/stderr separately
// Returns exit code, fills stdout_str and stderr_str
int execute_subprocess_with_pipes(
    const std::string& command_path,
    const std::vector<std::string>& args,
    std::string& stdout_str,
    std::string& stderr_str,
    const std::map<std::string, std::string>* env = nullptr
);

} // namespace runtime
} // namespace naab

