#ifndef NAAB_STACK_FRAME_H
#define NAAB_STACK_FRAME_H

#include <string>
#include <map>
#include <cstddef>

namespace naab {
namespace error {

/**
 * StackFrame: Represents a single call frame in a cross-language stack trace
 * (Phase 4.2.1)
 *
 * Captures function call information from any supported language:
 * - naab: Native NAAb function calls
 * - python: Python function calls via PyExecutor
 * - javascript: JS function calls via JsExecutor
 * - rust: Rust block calls via RustExecutor
 * - cpp: C++ block calls via CppExecutor
 */
struct StackFrame {
    std::string language;       // "naab", "python", "javascript", "rust", "cpp"
    std::string function_name;  // Name of function/method being called
    std::string filename;       // Source file (or "<native>" if unavailable)
    size_t line_number;         // Line number in source file (0 if unknown)

    // Optional: Local variables at time of call (for debugging)
    std::map<std::string, std::string> local_vars;

    StackFrame()
        : line_number(0) {}

    StackFrame(std::string lang, std::string func, std::string file = "", size_t line = 0)
        : language(std::move(lang)),
          function_name(std::move(func)),
          filename(std::move(file)),
          line_number(line) {}

    /**
     * Format stack frame as string for display
     *
     * Format: "  at {function_name} ({language}:{filename}:{line})"
     * Example: "  at add_numbers (python:math_utils.py:42)"
     */
    std::string toString() const {
        std::string result = "  at " + function_name;

        result += " (" + language + ":";

        if (!filename.empty()) {
            result += filename;
        } else {
            result += "<native>";
        }

        if (line_number > 0) {
            result += ":" + std::to_string(line_number);
        }

        result += ")";

        return result;
    }
};

} // namespace error
} // namespace naab

#endif // NAAB_STACK_FRAME_H
