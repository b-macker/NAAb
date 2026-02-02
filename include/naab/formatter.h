#ifndef NAAB_FORMATTER_PUBLIC_H
#define NAAB_FORMATTER_PUBLIC_H

// NAAb Auto-Formatter - Public API
// This is the public interface for the NAAb code formatter

#include <string>

namespace naab {
namespace formatter {

// Forward declarations
struct FormatterOptions;
class Formatter;

// Format NAAb source code with default options
std::string formatCode(const std::string& source_code);

// Format NAAb source code with custom options
std::string formatCode(const std::string& source_code, const FormatterOptions& options);

// Format NAAb file in-place
bool formatFile(const std::string& file_path);

// Format NAAb file with custom options
bool formatFile(const std::string& file_path, const FormatterOptions& options);

// Check if file is formatted correctly (for CI)
bool isFormatted(const std::string& source_code);
bool isFormatted(const std::string& source_code, const FormatterOptions& options);

} // namespace formatter
} // namespace naab

#endif // NAAB_FORMATTER_PUBLIC_H
