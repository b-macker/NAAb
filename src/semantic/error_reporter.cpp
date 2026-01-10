// NAAb Error Reporter Implementation
// Beautiful, helpful error messages with source context

#include "naab/error_reporter.h"
#include <fmt/core.h>
#include <sstream>
#include <algorithm>

namespace naab {
namespace error {

// Static member initialization (Phase 4.1.32)
bool Diagnostic::global_color_enabled_ = true;  // Enabled by default

// ANSI color codes
namespace colors {
    const char* RESET = "\033[0m";
    const char* RED = "\033[31m";
    const char* YELLOW = "\033[33m";
    const char* BLUE = "\033[34m";
    const char* GREEN = "\033[32m";
    const char* CYAN = "\033[36m";
    const char* BOLD = "\033[1m";
    const char* DIM = "\033[2m";
}

// ============================================================================
// Diagnostic Implementation
// ============================================================================

std::string Diagnostic::toString() const {
    std::ostringstream ss;

    // Severity label with color
    std::string sev_label;
    std::string color;

    switch (severity) {
        case Severity::Error:
            sev_label = "error";
            color = colors::RED;
            break;
        case Severity::Warning:
            sev_label = "warning";
            color = colors::YELLOW;
            break;
        case Severity::Info:
            sev_label = "info";
            color = colors::BLUE;
            break;
        case Severity::Hint:
            sev_label = "hint";
            color = colors::CYAN;
            break;
    }

    if (show_colors) {
        ss << color << colors::BOLD << sev_label << colors::RESET << ": ";
    } else {
        ss << sev_label << ": ";
    }

    ss << message;

    // Location info
    if (!filename.empty()) {
        ss << "\n  --> " << filename << ":" << line << ":" << column;
    } else {
        ss << "\n  --> line " << line << ", column " << column;
    }

    // Suggestions
    for (const auto& suggestion : suggestions) {
        if (show_colors) {
            ss << "\n  " << colors::GREEN << "help" << colors::RESET << ": " << suggestion;
        } else {
            ss << "\n  help: " << suggestion;
        }
    }

    return ss.str();
}

std::string Diagnostic::toStringWithSource(const std::string& source_code) const {
    // Basic version - will be enhanced by ErrorReporter
    return toString();
}

// ============================================================================
// ErrorReporter Implementation
// ============================================================================

ErrorReporter::ErrorReporter() {
}

void ErrorReporter::setSource(const std::string& source_code, const std::string& filename) {
    source_code_ = source_code;
    filename_ = filename;
    cacheSourceLines();
}

void ErrorReporter::error(const std::string& message, size_t line, size_t column) {
    report(Severity::Error, message, line, column);
}

void ErrorReporter::warning(const std::string& message, size_t line, size_t column) {
    report(Severity::Warning, message, line, column);
}

void ErrorReporter::info(const std::string& message, size_t line, size_t column) {
    report(Severity::Info, message, line, column);
}

void ErrorReporter::report(Severity severity, const std::string& message, size_t line, size_t column) {
    diagnostics_.emplace_back(severity, message, line, column, filename_);
}

void ErrorReporter::addSuggestion(const std::string& suggestion) {
    if (!diagnostics_.empty()) {
        diagnostics_.back().suggestions.push_back(suggestion);
    }
}

void ErrorReporter::addRelated(const Diagnostic& related) {
    if (!diagnostics_.empty()) {
        diagnostics_.back().related.push_back(related);
    }
}

bool ErrorReporter::hasErrors() const {
    return errorCount() > 0;
}

void ErrorReporter::printAll() const {
    for (const auto& diag : diagnostics_) {
        fmt::print("{}\n", diag.toString());
    }
}

void ErrorReporter::printAllWithSource() const {
    for (const auto& diag : diagnostics_) {
        fmt::print("{}\n", formatWithContext(diag));

        // Print related diagnostics
        for (const auto& related : diag.related) {
            fmt::print("\n{}\n", formatWithContext(related));
        }

        fmt::print("\n");  // Extra spacing between diagnostics
    }
}

void ErrorReporter::clear() {
    diagnostics_.clear();
}

size_t ErrorReporter::errorCount() const {
    return std::count_if(diagnostics_.begin(), diagnostics_.end(),
        [](const Diagnostic& d) { return d.severity == Severity::Error; });
}

size_t ErrorReporter::warningCount() const {
    return std::count_if(diagnostics_.begin(), diagnostics_.end(),
        [](const Diagnostic& d) { return d.severity == Severity::Warning; });
}

void ErrorReporter::cacheSourceLines() {
    source_lines_.clear();
    if (source_code_.empty()) return;

    std::istringstream stream(source_code_);
    std::string line;
    while (std::getline(stream, line)) {
        source_lines_.push_back(line);
    }
}

std::string ErrorReporter::getSourceLine(size_t line) const {
    if (line == 0 || line > source_lines_.size()) {
        return "";
    }
    return source_lines_[line - 1];  // Convert 1-indexed to 0-indexed
}

std::string ErrorReporter::formatWithContext(const Diagnostic& diag) const {
    std::ostringstream ss;

    // Header: severity + message
    std::string color = severityToColor(diag.severity);
    std::string sev_str = severityToString(diag.severity);

    if (diag.show_colors) {
        ss << color << colors::BOLD << sev_str << colors::RESET << ": ";
    } else {
        ss << sev_str << ": ";
    }
    ss << diag.message << "\n";

    // Location arrow
    if (!filename_.empty()) {
        if (diag.show_colors) {
            ss << "  " << colors::CYAN << "-->>" << colors::RESET << " "
               << filename_ << ":" << diag.line << ":" << diag.column << "\n";
        } else {
            ss << "  --> " << filename_ << ":" << diag.line << ":" << diag.column << "\n";
        }
    } else {
        if (diag.show_colors) {
            ss << "  " << colors::CYAN << "-->" << colors::RESET << " "
               << "line " << diag.line << ", column " << diag.column << "\n";
        } else {
            ss << "  --> line " << diag.line << ", column " << diag.column << "\n";
        }
    }

    // Source code context
    if (diag.show_source && !source_lines_.empty()) {
        size_t start_line = std::max(1ul, diag.line - diag.context_lines);
        size_t end_line = std::min(source_lines_.size(), diag.line + diag.context_lines);

        // Line number width for alignment
        size_t line_num_width = std::to_string(end_line).length();

        for (size_t i = start_line; i <= end_line; ++i) {
            std::string line = getSourceLine(i);
            bool is_error_line = (i == diag.line);

            // Gutter with line number
            if (diag.show_colors) {
                if (is_error_line) {
                    ss << fmt::format("  {} | ", colors::CYAN)
                       << fmt::format("{:{}}", i, line_num_width)
                       << colors::RESET << " ";
                } else {
                    ss << fmt::format("  {} | ", colors::DIM)
                       << fmt::format("{:{}}", i, line_num_width)
                       << colors::RESET << " ";
                }
            } else {
                ss << fmt::format("  {} | ", i);
            }

            ss << line << "\n";

            // Error marker (^ or ~~~)
            if (is_error_line && diag.column > 0) {
                // Spaces + gutter
                ss << fmt::format("  {:{}}", "", line_num_width + 3);

                // Spaces before caret
                size_t spaces = std::min(diag.column - 1, line.length());
                ss << std::string(spaces, ' ');

                // Caret with color
                if (diag.show_colors) {
                    ss << color << "^" << colors::RESET;
                } else {
                    ss << "^";
                }

                // Underline the rest of the token (simple heuristic)
                size_t underline_len = 0;
                if (diag.column <= line.length()) {
                    for (size_t j = diag.column - 1; j < line.length(); ++j) {
                        if (std::isalnum(line[j]) || line[j] == '_') {
                            underline_len++;
                        } else {
                            break;
                        }
                    }
                }

                if (underline_len > 0) {
                    if (diag.show_colors) {
                        ss << color << std::string(underline_len, '~') << colors::RESET;
                    } else {
                        ss << std::string(underline_len, '~');
                    }
                }

                ss << "\n";
            }
        }
    }

    // Suggestions
    for (const auto& suggestion : diag.suggestions) {
        if (diag.show_colors) {
            ss << "  " << colors::GREEN << colors::BOLD << "help" << colors::RESET << ": "
               << suggestion << "\n";
        } else {
            ss << "  help: " << suggestion << "\n";
        }
    }

    return ss.str();
}

std::string ErrorReporter::severityToString(Severity sev) const {
    switch (sev) {
        case Severity::Error: return "error";
        case Severity::Warning: return "warning";
        case Severity::Info: return "info";
        case Severity::Hint: return "hint";
    }
    return "unknown";
}

std::string ErrorReporter::severityToColor(Severity sev) const {
    switch (sev) {
        case Severity::Error: return colors::RED;
        case Severity::Warning: return colors::YELLOW;
        case Severity::Info: return colors::BLUE;
        case Severity::Hint: return colors::CYAN;
    }
    return colors::RESET;
}

// ============================================================================
// ErrorContext Integration (Phase 4.1)
// ============================================================================

ErrorContext ErrorReporter::createErrorContext(const Diagnostic& diag) const {
    ErrorContext ctx;
    ctx.filename = diag.filename.empty() ? filename_ : diag.filename;
    ctx.line = diag.line;
    ctx.column = diag.column;
    ctx.error_message = diag.message;
    ctx.source_line = getSourceLine(diag.line);

    // Copy suggestions
    if (!diag.suggestions.empty()) {
        ctx.suggestion = diag.suggestions[0];  // Use first suggestion as main
    }

    // Copy related diagnostics as notes
    for (const auto& related : diag.related) {
        ctx.notes.push_back(related.message);
    }

    return ctx;
}

void ErrorReporter::reportFromContext(const ErrorContext& ctx, Severity severity) {
    diagnostics_.emplace_back(severity, ctx.error_message, ctx.line, ctx.column, ctx.filename);

    // Add suggestion if present
    if (!ctx.suggestion.empty()) {
        diagnostics_.back().suggestions.push_back(ctx.suggestion);
    }

    // Add notes as additional suggestions
    for (const auto& note : ctx.notes) {
        diagnostics_.back().suggestions.push_back(note);
    }
}

} // namespace error
} // namespace naab
