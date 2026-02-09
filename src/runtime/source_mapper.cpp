// NAAb Source Mapper Implementation
// Maps error messages from temp files back to original NAAb source

#include "naab/source_mapper.h"
#include <fmt/core.h>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>

namespace naab {
namespace runtime {

SourceMapper::SourceMapper(const std::string& naab_file,
                          int heredoc_start_line,
                          int heredoc_start_column)
    : naab_file_(naab_file)
    , heredoc_start_line_(heredoc_start_line)
    , heredoc_start_column_(heredoc_start_column)
    , offset_(0) {
}

void SourceMapper::addMapping(int temp_line, int naab_line) {
    line_map_[temp_line] = naab_line;
}

std::optional<LineMapping> SourceMapper::mapLine(int temp_line) const {
    auto it = line_map_.find(temp_line);
    if (it != line_map_.end()) {
        return LineMapping{
            temp_line,
            it->second,
            heredoc_start_column_,
            naab_file_
        };
    }

    // If exact mapping not found, use offset calculation
    if (temp_line > offset_) {
        int naab_line = heredoc_start_line_ + (temp_line - offset_ - 1);
        return LineMapping{
            temp_line,
            naab_line,
            heredoc_start_column_,
            naab_file_
        };
    }

    return std::nullopt;
}

std::optional<int> SourceMapper::extractLineNumber(const std::string& error_line) const {
    // Match various compiler error formats:
    // /path/to/file.cpp:42:5: error: ...
    // --> file.rs:10:3  (Rust format)
    // Program.cs(15,10): error CS0103: ...

    std::smatch match;

    // Try Rust format: --> file.rs:line:col
    std::regex rust_pattern(R"(-->\s+[^:]+:(\d+):(\d+))");
    if (std::regex_search(error_line, match, rust_pattern)) {
        return std::stoi(match[1].str());
    }

    // Try C++ format: file:line:col:
    std::regex cpp_pattern(R"(:(\d+):(\d+):)");
    if (std::regex_search(error_line, match, cpp_pattern)) {
        return std::stoi(match[1].str());
    }

    // Try C# format: file(line,col):
    std::regex csharp_pattern(R"(\((\d+),(\d+)\):)");
    if (std::regex_search(error_line, match, csharp_pattern)) {
        return std::stoi(match[1].str());
    }

    return std::nullopt;
}

std::vector<std::string> SourceMapper::loadNaabSource() const {
    std::vector<std::string> lines;
    std::ifstream file(naab_file_);

    if (!file.is_open()) {
        return lines;
    }

    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
    }

    return lines;
}

std::string SourceMapper::getSourceContext(int naab_line, int context_lines) const {
    auto source_lines = loadNaabSource();

    if (source_lines.empty() || naab_line < 1 || naab_line > static_cast<int>(source_lines.size())) {
        return "";
    }

    std::ostringstream oss;

    // Calculate range (1-indexed to 0-indexed)
    int start = std::max(1, naab_line - context_lines);
    int end = std::min(static_cast<int>(source_lines.size()), naab_line + context_lines);

    for (int i = start; i <= end; i++) {
        std::string line = source_lines[i - 1];

        // Highlight the error line
        if (i == naab_line) {
            oss << fmt::format("  \033[1;31m{:4}\033[0m | {}\n", i, line);
            // Add caret indicator
            oss << "       | ";
            for (int j = 0; j < heredoc_start_column_ - 1; j++) {
                oss << " ";
            }
            oss << "\033[1;31m^\033[0m\n";
        } else {
            oss << fmt::format("  \033[2m{:4}\033[0m | {}\n", i, line);
        }
    }

    return oss.str();
}

std::string SourceMapper::translateError(const std::string& error_message) const {
    std::ostringstream result;
    std::istringstream stream(error_message);
    std::string line;

    bool first_error = true;

    while (std::getline(stream, line)) {
        auto line_num = extractLineNumber(line);

        if (line_num.has_value()) {
            auto mapping = mapLine(line_num.value());

            if (mapping.has_value()) {
                // This is an error we can map
                if (first_error) {
                    // Show NAAb file location
                    result << fmt::format("\n\033[1;31mError in {}:{}:{}\033[0m\n",
                                        mapping->naab_file,
                                        mapping->naab_line,
                                        mapping->naab_column);

                    // Show source context
                    result << getSourceContext(mapping->naab_line);
                    result << "\n";

                    first_error = false;
                }

                // Replace temp file path with NAAb file in error message
                std::string translated = line;

                // Extract just the error message (after line:col:)
                size_t msg_start = translated.find("error:");
                if (msg_start == std::string::npos) {
                    msg_start = translated.find("warning:");
                }

                if (msg_start != std::string::npos) {
                    std::string error_msg = translated.substr(msg_start);
                    result << fmt::format("  \033[1m{}\033[0m\n", error_msg);
                }
            } else {
                // Error in header/wrapper code (show original)
                result << line << "\n";
            }
        } else {
            // Not an error line (continuation, notes, etc.)
            // Check if it's a note or suggestion
            if (line.find("note:") != std::string::npos) {
                result << fmt::format("  \033[36m{}\033[0m\n", line);
            } else if (line.find("^") != std::string::npos ||
                      line.find("~") != std::string::npos) {
                // Skip caret lines from compiler (we show our own)
            } else if (!line.empty()) {
                result << "  " << line << "\n";
            }
        }
    }

    return result.str();
}

} // namespace runtime
} // namespace naab
