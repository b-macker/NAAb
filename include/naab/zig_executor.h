#pragma once

#include "naab/language_registry.h"
#include "naab/output_buffer.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace naab {
namespace interpreter {
    class Value;
}

namespace runtime {

/**
 * ZigExecutor: Executes Zig blocks via subprocess compilation
 *
 * Zig is a systems programming language (C replacement) that compiles to native code.
 * Offers manual memory management with safety features and excellent C interop.
 *
 * Example usage in NAAb:
 *   let result = <<zig
 *   const std = @import("std");
 *   const sum = 10 + 20;
 *   std.debug.print("{}", .{sum});
 *   >>
 */
class ZigExecutor : public Executor {
public:
    ZigExecutor();
    ~ZigExecutor() override = default;

    // Executor interface implementation
    bool execute(const std::string& code) override;
    std::shared_ptr<interpreter::Value> executeWithReturn(
        const std::string& code) override;
    std::shared_ptr<interpreter::Value> callFunction(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args
    ) override;
    bool isInitialized() const override { return true; }
    std::string getLanguage() const override { return "zig"; }
    std::string getCapturedOutput() override;

private:
    OutputBuffer stdout_buffer_;
    OutputBuffer stderr_buffer_;

    // Thread-safe temp file counter for parallel execution
    static std::atomic<int> temp_file_counter_;

    /**
     * Wrap code in proper Zig structure if needed
     *
     * @param code Raw Zig code
     * @param for_return If true, wrap last expression for return value output
     * @return Wrapped Zig code ready for compilation
     */
    std::string wrapZigCode(const std::string& code, bool for_return = false);
};

} // namespace runtime
} // namespace naab
