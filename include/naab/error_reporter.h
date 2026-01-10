#ifndef NAAB_ERROR_REPORTER_H
#define NAAB_ERROR_REPORTER_H

// NAAb Error Reporter - Enhanced error messages with context
// Provides beautiful, helpful error messages with source code context

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include "naab/error_context.h"

namespace naab {
namespace error {

// Error severity levels
enum class Severity {
    Error,
    Warning,
    Info,
    Hint
};

// Diagnostic message with location and context
class Diagnostic {
public:
    Diagnostic(Severity sev,
               const std::string& msg,
               size_t line,
               size_t column,
               const std::string& filename = "")
        : severity(sev),
          message(msg),
          line(line),
          column(column),
          filename(filename) {
        show_colors = global_color_enabled_;  // Respect global setting
    }

    // Global color setting (Phase 4.1.32)
    static void setGlobalColorEnabled(bool enabled) {
        global_color_enabled_ = enabled;
    }
    static bool isGlobalColorEnabled() {
        return global_color_enabled_;
    }

    Severity severity;
    std::string message;
    size_t line;
    size_t column;
    std::string filename;

    // Optional: Suggestions for fixes
    std::vector<std::string> suggestions;

    // Optional: Related context
    std::vector<Diagnostic> related;

    // Formatting options
    bool show_source = true;
    bool show_colors = true;
    size_t context_lines = 2;  // Lines before/after error

    std::string toString() const;
    std::string toStringWithSource(const std::string& source_code) const;

private:
    static bool global_color_enabled_;  // Global color setting (Phase 4.1.32)
};

// Error reporter with source code tracking
class ErrorReporter {
public:
    ErrorReporter();

    // Set source code for context
    void setSource(const std::string& source_code, const std::string& filename = "");

    // Report an error
    void error(const std::string& message, size_t line, size_t column);

    // Report a warning
    void warning(const std::string& message, size_t line, size_t column);

    // Report info
    void info(const std::string& message, size_t line, size_t column);

    // Report with custom severity
    void report(Severity severity, const std::string& message, size_t line, size_t column);

    // Add a suggestion to the last diagnostic
    void addSuggestion(const std::string& suggestion);

    // Add related context to the last diagnostic
    void addRelated(const Diagnostic& related);

    // Get all diagnostics
    const std::vector<Diagnostic>& getDiagnostics() const { return diagnostics_; }

    // Check if there are errors
    bool hasErrors() const;

    // Print all diagnostics
    void printAll() const;

    // Print with source context
    void printAllWithSource() const;

    // Clear all diagnostics
    void clear();

    // Get diagnostic counts
    size_t errorCount() const;
    size_t warningCount() const;

    // Create ErrorContext from diagnostic (Phase 4.1)
    ErrorContext createErrorContext(const Diagnostic& diag) const;

    // Report error using ErrorContext (Phase 4.1)
    void reportFromContext(const ErrorContext& ctx, Severity severity = Severity::Error);

private:
    std::vector<Diagnostic> diagnostics_;
    std::string source_code_;
    std::string filename_;
    std::vector<std::string> source_lines_;

    void cacheSourceLines();
    std::string getSourceLine(size_t line) const;
    std::string formatWithContext(const Diagnostic& diag) const;
    std::string severityToString(Severity sev) const;
    std::string severityToColor(Severity sev) const;
};

// ANSI color codes
namespace colors {
    extern const char* RESET;
    extern const char* RED;
    extern const char* YELLOW;
    extern const char* BLUE;
    extern const char* GREEN;
    extern const char* CYAN;
    extern const char* BOLD;
    extern const char* DIM;
}

} // namespace error
} // namespace naab

#endif // NAAB_ERROR_REPORTER_H
