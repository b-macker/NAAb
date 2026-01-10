// NAAb Stack Formatter Implementation
// Phase 4.2.6: Unified stack trace formatting with colors and JSON export

#include "naab/stack_formatter.h"
#include <sstream>
#include <fmt/core.h>

namespace naab {
namespace error {

std::string StackFormatter::getLanguageColor(const std::string& language) {
    if (language == "naab") {
        return COLOR_NAAB;  // Blue
    } else if (language == "python") {
        return COLOR_PYTHON;  // Green
    } else if (language == "javascript" || language == "js") {
        return COLOR_JAVASCRIPT;  // Yellow
    } else if (language == "rust") {
        return COLOR_RUST;  // Orange
    } else if (language == "cpp" || language == "c++") {
        return COLOR_CPP;  // Red
    } else {
        return COLOR_DEFAULT;  // White
    }
}

std::string StackFormatter::formatFrame(const StackFrame& frame, bool use_color) {
    std::ostringstream ss;

    if (use_color) {
        ss << getLanguageColor(frame.language);
    }

    ss << "  at " << frame.function_name;

    // Add file and line info
    if (!frame.filename.empty() && frame.filename != "<unknown>") {
        ss << " (" << frame.filename;
        if (frame.line_number > 0) {
            ss << ":" << frame.line_number;
        }
        ss << ")";
    }

    // Add language tag
    ss << " [" << frame.language << "]";

    // Add local variables if available
    if (!frame.local_vars.empty()) {
        ss << "\n    Variables: {";
        bool first = true;
        for (const auto& [name, value] : frame.local_vars) {
            if (!first) ss << ", ";
            ss << name << "=" << value;
            first = false;
        }
        ss << "}";
    }

    if (use_color) {
        ss << COLOR_RESET;
    }

    return ss.str();
}

std::string StackFormatter::formatColored(const std::vector<StackFrame>& frames) {
    if (frames.empty()) {
        return "<empty stack trace>";
    }

    std::ostringstream ss;
    ss << "Stack trace (most recent call last):\n";

    // Iterate in reverse order (most recent first)
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
        ss << formatFrame(*it, true) << "\n";
    }

    return ss.str();
}

std::string StackFormatter::formatPlain(const std::vector<StackFrame>& frames) {
    if (frames.empty()) {
        return "<empty stack trace>";
    }

    std::ostringstream ss;
    ss << "Stack trace (most recent call last):\n";

    // Iterate in reverse order (most recent first)
    for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
        ss << formatFrame(*it, false) << "\n";
    }

    return ss.str();
}

std::string StackFormatter::formatJSON(const std::vector<StackFrame>& frames) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"stack_trace\": [\n";

    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& frame = frames[i];

        ss << "    {\n";
        ss << "      \"language\": \"" << frame.language << "\",\n";
        ss << "      \"function\": \"" << frame.function_name << "\",\n";
        ss << "      \"file\": \"" << frame.filename << "\",\n";
        ss << "      \"line\": " << frame.line_number;

        // Add local variables if available
        if (!frame.local_vars.empty()) {
            ss << ",\n      \"local_vars\": {\n";
            bool first = true;
            for (const auto& [name, value] : frame.local_vars) {
                if (!first) ss << ",\n";
                ss << "        \"" << name << "\": \"" << value << "\"";
                first = false;
            }
            ss << "\n      }";
        }

        ss << "\n    }";

        if (i < frames.size() - 1) {
            ss << ",";
        }
        ss << "\n";
    }

    ss << "  ],\n";
    ss << "  \"frame_count\": " << frames.size() << "\n";
    ss << "}\n";

    return ss.str();
}

} // namespace error
} // namespace naab
