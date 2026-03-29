#pragma once

// NAAb Persistent Shell Executor
// Keeps a bash process alive via pipes, preserving state between blocks.

#include "naab/persistent_process_executor.h"

namespace naab {
namespace runtime {

class PersistentShellExecutor : public PersistentProcessExecutor {
public:
    PersistentShellExecutor();
    ~PersistentShellExecutor() override { stop(); }

protected:
    std::string wrapCodeForExecution(const std::string& code) const override;
    std::string getSentinel() const override;
    std::string getStartupCode() const override;
    std::string getExitCommand() const override;
    interpreter::NaabVal parseOutput(
        const std::string& stdout_text, const std::string& stderr_text,
        int implicit_exit_code) const override;
};

} // namespace runtime
} // namespace naab
