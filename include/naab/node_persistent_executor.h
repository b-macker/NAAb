#pragma once

// NAAb Node.js Persistent Executor
// Keeps a Node.js process alive via pipes, enabling npm access and state persistence.

#include "naab/persistent_process_executor.h"

namespace naab {
namespace runtime {

class NodePersistentExecutor : public PersistentProcessExecutor {
public:
    NodePersistentExecutor();

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
