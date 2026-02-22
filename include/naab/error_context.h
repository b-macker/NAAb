#pragma once

#include <string>
#include <vector>
#include <cstddef>

namespace naab {
namespace error {

/**
 * ErrorContext: Rich error information for user-friendly error reporting
 *
 * Captures source location, code snippets, and suggestions for better
 * error messages (Phase 4.1)
 */
struct ErrorContext {
    std::string filename;
    size_t line;
    size_t column;
    std::string source_line;
    std::string error_message;
    std::string suggestion;
    std::vector<std::string> notes;

    ErrorContext() : line(0), column(0) {}

    ErrorContext(std::string file, size_t ln, size_t col,
                 std::string msg = "", std::string src = "")
        : filename(std::move(file)),
          line(ln),
          column(col),
          source_line(std::move(src)),
          error_message(std::move(msg)) {}
};

} // namespace error
} // namespace naab

