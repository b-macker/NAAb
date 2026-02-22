#pragma once
//
// NAAb Error Formatter Utility
// Provides standardized error message formatting for consistent, helpful errors
//


#include <string>
#include <vector>
#include <sstream>

namespace naab {
namespace utils {

/**
 * ErrorFormatter - Utility class for creating consistent, helpful error messages
 *
 * All error messages follow the 5 principles:
 * 1. Explain WHAT went wrong
 * 2. Explain WHY it's wrong
 * 3. Show HOW to fix it
 * 4. Give EXAMPLES (✗ Wrong vs ✓ Right)
 * 5. Suggest ALTERNATIVES when available
 */
class ErrorFormatter {
public:
    /**
     * Format a function argument count error
     *
     * @param function_name The full function name (e.g., "array.push")
     * @param param_names List of parameter names (e.g., {"array", "element"})
     * @param expected_count Expected number of arguments
     * @param provided_count Actual number of arguments provided
     * @return Formatted error message
     */
    static std::string formatArgumentError(
        const std::string& function_name,
        const std::vector<std::string>& param_names,
        int expected_count,
        int provided_count);

    /**
     * Format a type mismatch error
     *
     * @param operation The operation being performed (e.g., "array.length")
     * @param expected_type Expected type name (e.g., "array")
     * @param actual_type Actual type name (e.g., "string")
     * @param actual_value String representation of the actual value
     * @return Formatted error message
     */
    static std::string formatTypeMismatch(
        const std::string& operation,
        const std::string& expected_type,
        const std::string& actual_type,
        const std::string& actual_value);

    /**
     * Format an empty collection error
     *
     * @param operation The operation being performed (e.g., "array.pop")
     * @param collection_type Type of collection (e.g., "array", "dict")
     * @param check_function Function to check collection size (e.g., "array.length")
     * @return Formatted error message
     */
    static std::string formatEmptyCollectionError(
        const std::string& operation,
        const std::string& collection_type,
        const std::string& check_function);

    /**
     * Format a domain validation error
     *
     * @param function_name The function name (e.g., "math.sqrt")
     * @param constraint Description of the constraint (e.g., "requires non-negative argument")
     * @param actual_value String representation of the invalid value
     * @return Formatted error message
     */
    static std::string formatDomainError(
        const std::string& function_name,
        const std::string& constraint,
        const std::string& actual_value);

    /**
     * Generic error formatter with custom help text and examples
     *
     * @param title Error title (e.g., "Type error: Cannot call non-function value")
     * @param problem Description of what went wrong
     * @param help Help text (can be multi-line with bullet points)
     * @param wrong_example Example of incorrect usage
     * @param right_example Example of correct usage
     * @return Formatted error message
     */
    static std::string format(
        const std::string& title,
        const std::string& problem,
        const std::string& help,
        const std::string& wrong_example,
        const std::string& right_example);

private:
    /**
     * Indent all lines of text by specified number of spaces
     *
     * @param text Text to indent
     * @param spaces Number of spaces to indent
     * @return Indented text
     */
    static std::string indent(const std::string& text, int spaces);

    /**
     * Wrap text to specified line width
     *
     * @param text Text to wrap
     * @param width Maximum line width
     * @return Wrapped text
     */
    static std::string wrapText(const std::string& text, int width);
};

} // namespace utils
} // namespace naab

