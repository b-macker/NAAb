//
// NAAb Error Formatter Utility - Implementation
//

#include "naab/utils/error_formatter.h"
#include <sstream>
#include <algorithm>

namespace naab {
namespace utils {

std::string ErrorFormatter::formatArgumentError(
    const std::string& function_name,
    const std::vector<std::string>& param_names,
    int expected_count,
    int provided_count)
{
    std::ostringstream oss;

    // Title
    oss << "Argument error: " << function_name << "(";
    for (size_t i = 0; i < param_names.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << param_names[i];
    }
    oss << ") expects " << expected_count << " argument";
    if (expected_count != 1) oss << "s";
    oss << ", got " << provided_count << "\n\n";

    // Parameter descriptions (if any)
    if (!param_names.empty()) {
        oss << "  Parameters:\n";
        for (const auto& param : param_names) {
            oss << "  - " << param << "\n";
        }
        oss << "\n";
    }

    // Help text
    oss << "  Help:\n";
    oss << "  - Provide exactly " << expected_count << " argument";
    if (expected_count != 1) oss << "s";
    oss << "\n";
    oss << "  - Check the function signature above\n";
    oss << "\n";

    // Example
    oss << "  Example:\n";
    if (provided_count < expected_count) {
        oss << "    ✗ Wrong: " << function_name << "(";
        for (int i = 0; i < provided_count; ++i) {
            if (i > 0) oss << ", ";
            oss << "arg" << (i + 1);
        }
        oss << ")  // too few arguments\n";
    } else {
        oss << "    ✗ Wrong: " << function_name << "(";
        for (int i = 0; i < provided_count; ++i) {
            if (i > 0) oss << ", ";
            oss << "arg" << (i + 1);
        }
        oss << ")  // too many arguments\n";
    }
    oss << "    ✓ Right: " << function_name << "(";
    for (int i = 0; i < expected_count; ++i) {
        if (i > 0) oss << ", ";
        oss << "arg" << (i + 1);
    }
    oss << ")\n";

    return oss.str();
}

std::string ErrorFormatter::formatTypeMismatch(
    const std::string& operation,
    const std::string& expected_type,
    const std::string& actual_type,
    const std::string& actual_value)
{
    std::ostringstream oss;

    // Title
    oss << "Type error: " << operation << " requires " << expected_type << "\n\n";

    // What was provided
    oss << "  Got: " << actual_type;
    if (!actual_value.empty()) {
        oss << " = " << actual_value;
    }
    oss << "\n";
    oss << "  Expected: " << expected_type << "\n\n";

    // Help text
    oss << "  Help:\n";
    oss << "  - This operation only works with " << expected_type << "\n";
    oss << "  - Use typeof() or debug.type() to check types\n";
    oss << "\n";

    // Example
    oss << "  Example:\n";
    oss << "    ✗ Wrong: " << operation << "(" << actual_value << ")  // " << actual_type << "\n";
    oss << "    ✓ Right: " << operation << "(<" << expected_type << ">)\n";

    return oss.str();
}

std::string ErrorFormatter::formatEmptyCollectionError(
    const std::string& operation,
    const std::string& collection_type,
    const std::string& check_function)
{
    std::ostringstream oss;

    // Title
    oss << "Error: " << operation << " requires a non-empty " << collection_type << "\n\n";

    // Problem
    oss << "  Attempted to " << operation << " from empty " << collection_type << "\n\n";

    // Help text
    oss << "  Help:\n";
    oss << "  - Check if " << collection_type << " has elements before operating\n";
    oss << "  - Use " << check_function << " to check size\n";
    oss << "\n";

    // Example
    oss << "  Example:\n";
    oss << "    ✗ Wrong: " << operation << "([])  // empty " << collection_type << "\n";
    oss << "    ✓ Right: if " << check_function << "(coll) > 0 { " << operation << "(coll) }\n";

    return oss.str();
}

std::string ErrorFormatter::formatDomainError(
    const std::string& function_name,
    const std::string& constraint,
    const std::string& actual_value)
{
    std::ostringstream oss;

    // Title
    oss << "Domain error: " << function_name << " " << constraint << "\n\n";

    // What was provided
    oss << "  Got: " << actual_value << "\n";
    oss << "  Constraint: " << constraint << "\n\n";

    // Help text
    oss << "  Help:\n";
    oss << "  - Validate input before calling " << function_name << "\n";
    oss << "  - Check the mathematical domain requirements\n";
    oss << "\n";

    // Example
    oss << "  Example:\n";
    oss << "    ✗ Wrong: " << function_name << "(" << actual_value << ")  // violates constraint\n";
    oss << "    ✓ Right: Add validation before calling " << function_name << "\n";

    return oss.str();
}

std::string ErrorFormatter::format(
    const std::string& title,
    const std::string& problem,
    const std::string& help,
    const std::string& wrong_example,
    const std::string& right_example)
{
    std::ostringstream oss;

    // Title
    oss << title << "\n\n";

    // Problem (if provided)
    if (!problem.empty()) {
        oss << "  " << problem << "\n\n";
    }

    // Help text
    if (!help.empty()) {
        oss << "  Help:\n";
        // Split help into lines and indent each
        std::istringstream help_stream(help);
        std::string line;
        while (std::getline(help_stream, line)) {
            if (!line.empty()) {
                oss << "  " << line << "\n";
            }
        }
        oss << "\n";
    }

    // Examples
    if (!wrong_example.empty() || !right_example.empty()) {
        oss << "  Example:\n";
        if (!wrong_example.empty()) {
            oss << "    ✗ Wrong: " << wrong_example << "\n";
        }
        if (!right_example.empty()) {
            oss << "    ✓ Right: " << right_example << "\n";
        }
    }

    return oss.str();
}

std::string ErrorFormatter::indent(const std::string& text, int spaces) {
    std::string indentation(spaces, ' ');
    std::ostringstream oss;
    std::istringstream iss(text);
    std::string line;
    bool first = true;

    while (std::getline(iss, line)) {
        if (!first) {
            oss << "\n";
        }
        oss << indentation << line;
        first = false;
    }

    return oss.str();
}

std::string ErrorFormatter::wrapText(const std::string& text, int width) {
    std::ostringstream oss;
    std::istringstream iss(text);
    std::string word;
    int line_length = 0;

    while (iss >> word) {
        int word_length = static_cast<int>(word.length());
        if (line_length + word_length + 1 > width) {
            oss << "\n" << word;
            line_length = word_length;
        } else {
            if (line_length > 0) {
                oss << " ";
                line_length++;
            }
            oss << word;
            line_length += word_length;
        }
    }

    return oss.str();
}

} // namespace utils
} // namespace naab
