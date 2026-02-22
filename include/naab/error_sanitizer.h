#pragma once
// NAAb Error Sanitizer
// Week 5, Task 5.2: Error Message Scrubbing
//
// Prevents information leakage through error messages by:
// - Sanitizing stack traces (removing absolute paths)
// - Redacting variable values in production mode
// - Removing internal implementation details
// - Preventing sensitive data exposure


#include <string>
#include <vector>
#include <regex>
#include <optional>

namespace naab {
namespace error {

// ============================================================================
// Error Sanitization Modes
// ============================================================================

enum class SanitizationMode {
    DEVELOPMENT,   // Show all details (for debugging)
    PRODUCTION,    // Scrub sensitive information
    STRICT         // Minimal information only
};

// ============================================================================
// Error Sanitizer
// ============================================================================

class ErrorSanitizer {
public:
    /**
     * Sanitize error message for public display
     *
     * Removes:
     * - Absolute file paths (convert to relative)
     * - Variable values (in production/strict mode)
     * - Memory addresses
     * - Internal type names
     * - Stack frame internals
     */
    static std::string sanitize(
        const std::string& error_msg,
        SanitizationMode mode = SanitizationMode::PRODUCTION
    );

    /**
     * Sanitize stack trace
     *
     * Removes:
     * - Absolute paths
     * - Memory addresses
     * - Internal function names (in strict mode)
     */
    static std::string sanitizeStackTrace(
        const std::string& stack_trace,
        SanitizationMode mode = SanitizationMode::PRODUCTION
    );

    /**
     * Redact variable values from error message
     *
     * Example:
     *   "Variable 'password' has value 'secret123'"
     *   → "Variable 'password' has value '<redacted>'"
     */
    static std::string redactValues(
        const std::string& msg,
        SanitizationMode mode = SanitizationMode::PRODUCTION
    );

    /**
     * Remove absolute file paths, leaving only relative paths
     *
     * Example:
     *   "/home/user/project/src/main.naab"
     *   → "src/main.naab"
     */
    static std::string sanitizeFilePaths(const std::string& msg);

    /**
     * Remove memory addresses from error messages
     *
     * Example:
     *   "Object at 0x7fff12345678"
     *   → "Object at <address>"
     */
    static std::string sanitizeAddresses(const std::string& msg);

    /**
     * Sanitize type information (remove internal C++ types)
     *
     * Example:
     *   "std::shared_ptr<naab::interpreter::Value>"
     *   → "Value"
     */
    static std::string sanitizeTypeNames(const std::string& msg);

    /**
     * Check if error message contains sensitive patterns
     *
     * Returns list of potential sensitive information found
     */
    static std::vector<std::string> detectSensitiveInfo(const std::string& msg);

    /**
     * Set global sanitization mode
     */
    static void setMode(SanitizationMode mode);

    /**
     * Get current sanitization mode
     */
    static SanitizationMode getMode();

    /**
     * Set project root for path sanitization
     *
     * Used to convert absolute paths to relative paths
     */
    static void setProjectRoot(const std::string& root);

private:
    // Current sanitization mode (defaults to PRODUCTION)
    static SanitizationMode mode_;

    // Project root for path sanitization
    static std::string project_root_;

    // Regex patterns for sensitive data
    static const std::vector<std::regex> sensitive_patterns_;

    // Helper: Convert absolute path to relative
    static std::string makePathRelative(const std::string& path);

    // Helper: Redact a matched sensitive pattern
    static std::string redactPattern(const std::string& text, const std::regex& pattern);
};

// ============================================================================
// RAII Guard for Error Sanitization
// ============================================================================

/**
 * RAII guard to temporarily change sanitization mode
 *
 * Usage:
 *   {
 *       ErrorSanitizationGuard guard(SanitizationMode::DEVELOPMENT);
 *       // Error messages now show full details
 *       throw RuntimeException("Detailed error");
 *   }
 *   // Mode restored to previous value
 */
class ErrorSanitizationGuard {
public:
    explicit ErrorSanitizationGuard(SanitizationMode mode)
        : previous_mode_(ErrorSanitizer::getMode()) {
        ErrorSanitizer::setMode(mode);
    }

    ~ErrorSanitizationGuard() {
        ErrorSanitizer::setMode(previous_mode_);
    }

    // Non-copyable, non-movable
    ErrorSanitizationGuard(const ErrorSanitizationGuard&) = delete;
    ErrorSanitizationGuard& operator=(const ErrorSanitizationGuard&) = delete;

private:
    SanitizationMode previous_mode_;
};

// ============================================================================
// Sensitive Data Patterns
// ============================================================================

/**
 * Common patterns that might indicate sensitive data
 */
namespace patterns {

// API keys, tokens (e.g., "token: abc123...")
inline const char* API_KEY = R"((?:api[_-]?key|token|secret|password|auth)[:\s]*['"]?([a-zA-Z0-9_-]{16,})['"]?)";

// Email addresses
inline const char* EMAIL = R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})";

// Credit card numbers (simple pattern)
inline const char* CREDIT_CARD = R"(\b\d{4}[\s-]?\d{4}[\s-]?\d{4}[\s-]?\d{4}\b)";

// IP addresses
inline const char* IP_ADDRESS = R"(\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b)";

// File paths (absolute)
inline const char* ABSOLUTE_PATH = R"((?:\/[a-zA-Z0-9._-]+)+)";

// Memory addresses (hex)
inline const char* MEMORY_ADDRESS = R"(0x[0-9a-fA-F]{8,16})";

// Variable values in quotes
inline const char* QUOTED_VALUE = R"((?:value|content|data)['"]?\s*[:=]\s*['"]([^'"]+)['"])";

} // namespace patterns

} // namespace error
} // namespace naab

