#ifndef NAAB_CSHARP_EXECUTOR_H
#define NAAB_CSHARP_EXECUTOR_H

#include "naab/language_registry.h"
#include "naab/output_buffer.h"
#include <memory>
#include <string>
#include <vector>

namespace naab {
namespace interpreter {
    class Value;
}

namespace runtime {

class CSharpExecutor : public Executor {
public:
    CSharpExecutor();
    ~CSharpExecutor() override = default;

    bool execute(const std::string& code) override;
    std::shared_ptr<interpreter::Value> executeWithReturn(
        const std::string& code) override;
    std::shared_ptr<interpreter::Value> callFunction(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args
    ) override;
    bool isInitialized() const override { return true; }
    std::string getLanguage() const override { return "csharp"; }
    std::string getCapturedOutput() override;

private:
    OutputBuffer stdout_buffer_;
    OutputBuffer stderr_buffer_;
};

} // namespace runtime
} // namespace naab

#endif // NAAB_CSHARP_EXECUTOR_H
