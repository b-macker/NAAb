#ifndef NAAB_SOURCE_MAPPER_H
#define NAAB_SOURCE_MAPPER_H

// NAAb Source Mapper
// Maps lines between generated temp files and original NAAb source

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace naab {
namespace runtime {

// Represents a mapping between temp file line and NAAb source line
struct LineMapping {
    int temp_line;        // Line number in generated temp file
    int naab_line;        // Line number in original NAAb file
    int naab_column;      // Column in NAAb file (start of heredoc)
    std::string naab_file; // NAAb source file path
};

// Source mapper for polyglot blocks
class SourceMapper {
public:
    SourceMapper(const std::string& naab_file, int heredoc_start_line, int heredoc_start_column);

    // Record that temp_line corresponds to naab_line
    void addMapping(int temp_line, int naab_line);

    // Map temp file line number to NAAb line number
    std::optional<LineMapping> mapLine(int temp_line) const;

    // Translate error messages from temp file to NAAb file
    std::string translateError(const std::string& error_message) const;

    // Get NAAb source context for error display
    std::string getSourceContext(int naab_line, int context_lines = 2) const;

    // Set the offset (number of lines before user code in temp file)
    void setOffset(int offset) { offset_ = offset; }

    // Get the offset
    int getOffset() const { return offset_; }

private:
    std::string naab_file_;
    int heredoc_start_line_;
    int heredoc_start_column_;
    int offset_;  // Number of header/wrapper lines before user code
    std::unordered_map<int, int> line_map_;  // temp_line -> naab_line

    // Extract line number from compiler error message
    std::optional<int> extractLineNumber(const std::string& error_line) const;

    // Load NAAb source file for context display
    std::vector<std::string> loadNaabSource() const;
};

} // namespace runtime
} // namespace naab

#endif // NAAB_SOURCE_MAPPER_H
