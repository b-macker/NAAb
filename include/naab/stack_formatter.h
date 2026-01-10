#ifndef NAAB_STACK_FORMATTER_H
#define NAAB_STACK_FORMATTER_H

// Phase 4.2.6: Unified Stack Trace Formatting
// Enhanced formatting with color coding and JSON export

#include "naab/stack_frame.h"
#include <string>
#include <vector>

namespace naab {
namespace error {

class StackFormatter {
public:
    // Format stack trace with color coding
    static std::string formatColored(const std::vector<StackFrame>& frames);

    // Format stack trace without colors (for CI/logs)
    static std::string formatPlain(const std::vector<StackFrame>& frames);

    // Export stack trace as JSON
    static std::string formatJSON(const std::vector<StackFrame>& frames);

    // Get ANSI color code for language
    static std::string getLanguageColor(const std::string& language);

    // Format single frame with color
    static std::string formatFrame(const StackFrame& frame, bool use_color = true);

private:
    // ANSI color codes
    static constexpr const char* COLOR_RESET = "\033[0m";
    static constexpr const char* COLOR_NAAB = "\033[34m";      // Blue
    static constexpr const char* COLOR_PYTHON = "\033[32m";    // Green
    static constexpr const char* COLOR_JAVASCRIPT = "\033[33m"; // Yellow
    static constexpr const char* COLOR_RUST = "\033[38;5;208m"; // Orange
    static constexpr const char* COLOR_CPP = "\033[31m";       // Red
    static constexpr const char* COLOR_DEFAULT = "\033[37m";   // White
};

} // namespace error
} // namespace naab

#endif // NAAB_STACK_FORMATTER_H
