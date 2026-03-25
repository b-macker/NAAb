#pragma once

// NAAb Python Executor Adapter
// Adapts PythonCExecutor to the Executor interface for the language registry

#include "naab/language_registry.h"
#include "naab/python_c_executor.h"
#include <memory>
#include <string>

namespace naab {
namespace runtime {

// Adapter class that wraps PythonCExecutor for the language registry
class PyExecutorAdapter : public Executor {
public:
    PyExecutorAdapter();
    ~PyExecutorAdapter() override = default;

    // Execute code and store in runtime context
    bool execute(const std::string& code) override;

    // Phase 2.3: Execute code and return the result value
    interpreter::NaabVal executeWithReturn(
        const std::string& code) override;

    // Call a function in the executor
    interpreter::NaabVal callFunction(
        const std::string& function_name,
        const std::vector<interpreter::NaabVal>& args
    ) override;

    // Check if executor is initialized
    bool isInitialized() const override;

    // Get language name
    std::string getLanguage() const override { return "python"; }

    // Get captured output
    std::string getCapturedOutput() override;

private:
    std::unique_ptr<PythonCExecutor> executor_;
};

} // namespace runtime
} // namespace naab

