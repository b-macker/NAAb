#pragma once

// NAAb Persistent Ruby Executor
// Keeps a Ruby process alive via a custom eval loop over stdin.

#include "naab/persistent_process_executor.h"

namespace naab {
namespace runtime {

class PersistentRubyExecutor : public PersistentProcessExecutor {
public:
    PersistentRubyExecutor();

protected:
    std::string wrapCodeForExecution(const std::string& code) const override;
    std::string getSentinel() const override;
    std::string getStartupCode() const override;
    std::string getExitCommand() const override;
};

} // namespace runtime
} // namespace naab
