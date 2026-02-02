// NAAb Error Sanitizer Implementation
// Week 5, Task 5.2: Error Message Scrubbing

#include "naab/error_sanitizer.h"
#include <fmt/core.h>
#include <algorithm>
#include <filesystem>

namespace naab {
namespace error {

// Static member initialization
SanitizationMode ErrorSanitizer::mode_ = SanitizationMode::PRODUCTION;
std::string ErrorSanitizer::project_root_ = "";

const std::vector<std::regex> ErrorSanitizer::sensitive_patterns_ = {
    std::regex(patterns::API_KEY, std::regex::icase),
    std::regex(patterns::EMAIL),
    std::regex(patterns::CREDIT_CARD),
    std::regex(patterns::MEMORY_ADDRESS),
    std::regex(patterns::QUOTED_VALUE),
};

// ============================================================================
// Main Sanitization
// ============================================================================

std::string ErrorSanitizer::sanitize(
    const std::string& error_msg,
    SanitizationMode mode
) {
    if (mode == SanitizationMode::DEVELOPMENT) {
        // In development, show everything
        return error_msg;
    }

    std::string sanitized = error_msg;

    // Apply sanitization steps
    sanitized = sanitizeFilePaths(sanitized);
    sanitized = sanitizeAddresses(sanitized);

    if (mode == SanitizationMode::PRODUCTION) {
        sanitized = redactValues(sanitized, mode);
        sanitized = sanitizeTypeNames(sanitized);
    } else if (mode == SanitizationMode::STRICT) {
        sanitized = redactValues(sanitized, mode);
        sanitized = sanitizeTypeNames(sanitized);

        // In strict mode, also remove line numbers and column numbers
        // to prevent information leakage about code structure
        std::regex line_col_pattern(R"(:(\d+):(\d+))");
        sanitized = std::regex_replace(sanitized, line_col_pattern, ":<line>:<col>");
    }

    return sanitized;
}

// ============================================================================
// Stack Trace Sanitization
// ============================================================================

std::string ErrorSanitizer::sanitizeStackTrace(
    const std::string& stack_trace,
    SanitizationMode mode
) {
    if (mode == SanitizationMode::DEVELOPMENT) {
        return stack_trace;
    }

    std::string sanitized = stack_trace;

    // Remove absolute paths
    sanitized = sanitizeFilePaths(sanitized);

    // Remove memory addresses
    sanitized = sanitizeAddresses(sanitized);

    // Remove internal function names in strict mode
    if (mode == SanitizationMode::STRICT) {
        // Replace internal namespace prefixes
        std::regex internal_ns(R"(naab::(?:interpreter|parser|lexer|internal)::)");
        sanitized = std::regex_replace(sanitized, internal_ns, "");

        // Remove template parameters
        std::regex template_params(R"(<[^>]+>)");
        sanitized = std::regex_replace(sanitized, template_params, "<...>");
    }

    return sanitized;
}

// ============================================================================
// Value Redaction
// ============================================================================

std::string ErrorSanitizer::redactValues(
    const std::string& msg,
    SanitizationMode mode
) {
    if (mode == SanitizationMode::DEVELOPMENT) {
        return msg;
    }

    std::string redacted = msg;

    // Redact sensitive patterns
    for (const auto& pattern : sensitive_patterns_) {
        redacted = redactPattern(redacted, pattern);
    }

    // Redact values in quotes (e.g., "value: 'secret123'")
    std::regex value_pattern(R"((value|content|data)['"]?\s*[:=]\s*['"]([^'"]+)['"])");
    redacted = std::regex_replace(redacted, value_pattern, "$1: <redacted>");

    // Redact variable assignments (e.g., "password = 'secret'")
    std::regex assignment_pattern(R"(([a-zA-Z_][a-zA-Z0-9_]*)\s*=\s*['"]([^'"]+)['"])");
    redacted = std::regex_replace(redacted, assignment_pattern, "$1 = <redacted>");

    return redacted;
}

// ============================================================================
// File Path Sanitization
// ============================================================================

std::string ErrorSanitizer::sanitizeFilePaths(const std::string& msg) {
    std::string sanitized = msg;

    // If project root is set, make paths relative
    if (!project_root_.empty()) {
        size_t pos = 0;
        while ((pos = sanitized.find(project_root_, pos)) != std::string::npos) {
            sanitized.replace(pos, project_root_.length(), "");
            pos += 1;  // Move past the replacement
        }
    }

    // Remove common absolute path prefixes
    std::vector<std::string> prefixes = {
        "/home/",
        "/usr/",
        "C:\\Users\\",
        "C:\\Program Files\\",
        "/data/data/",  // Android paths
    };

    for (const auto& prefix : prefixes) {
        std::regex prefix_pattern(prefix + R"([^\s:]+)");
        sanitized = std::regex_replace(sanitized, prefix_pattern, [&](const std::smatch& match) {
            std::string path = match.str();
            // Keep only the last 2-3 components
            std::filesystem::path fs_path(path);
            std::filesystem::path relative_path;

            int components = 0;
            for (auto it = fs_path.end(); it != fs_path.begin() && components < 3; --it) {
                if (it != fs_path.end()) {
                    relative_path = *it / relative_path;
                    components++;
                }
            }

            return relative_path.string();
        });
    }

    return sanitized;
}

// ============================================================================
// Address Sanitization
// ============================================================================

std::string ErrorSanitizer::sanitizeAddresses(const std::string& msg) {
    // Replace memory addresses (0x...) with <address>
    std::regex addr_pattern(R"(0x[0-9a-fA-F]{8,16})");
    return std::regex_replace(msg, addr_pattern, "<address>");
}

// ============================================================================
// Type Name Sanitization
// ============================================================================

std::string ErrorSanitizer::sanitizeTypeNames(const std::string& msg) {
    std::string sanitized = msg;

    // Remove std:: prefix
    std::regex std_prefix(R"(std::)");
    sanitized = std::regex_replace(sanitized, std_prefix, "");

    // Remove naab:: prefix
    std::regex naab_prefix(R"(naab::(?:interpreter|parser|lexer|runtime)::)");
    sanitized = std::regex_replace(sanitized, naab_prefix, "");

    // Simplify shared_ptr<T> to T
    std::regex shared_ptr_pattern(R"(shared_ptr<([^>]+)>)");
    sanitized = std::regex_replace(sanitized, shared_ptr_pattern, "$1");

    // Simplify vector<T> to [T]
    std::regex vector_pattern(R"(vector<([^>]+)>)");
    sanitized = std::regex_replace(sanitized, vector_pattern, "[$1]");

    // Remove template parameters for complex types
    std::regex complex_template(R"(([a-zA-Z_][a-zA-Z0-9_]*)<[^>]*<[^>]*>[^>]*>)");
    sanitized = std::regex_replace(sanitized, complex_template, "$1<...>");

    return sanitized;
}

// ============================================================================
// Sensitive Info Detection
// ============================================================================

std::vector<std::string> ErrorSanitizer::detectSensitiveInfo(const std::string& msg) {
    std::vector<std::string> findings;

    // Check for API keys/tokens
    std::regex api_key_pattern(patterns::API_KEY, std::regex::icase);
    if (std::regex_search(msg, api_key_pattern)) {
        findings.push_back("Potential API key or token");
    }

    // Check for email addresses
    std::regex email_pattern(patterns::EMAIL);
    if (std::regex_search(msg, email_pattern)) {
        findings.push_back("Email address");
    }

    // Check for credit card numbers
    std::regex cc_pattern(patterns::CREDIT_CARD);
    if (std::regex_search(msg, cc_pattern)) {
        findings.push_back("Potential credit card number");
    }

    // Check for IP addresses
    std::regex ip_pattern(patterns::IP_ADDRESS);
    if (std::regex_search(msg, ip_pattern)) {
        findings.push_back("IP address");
    }

    // Check for absolute paths
    std::regex path_pattern(patterns::ABSOLUTE_PATH);
    if (std::regex_search(msg, path_pattern)) {
        findings.push_back("Absolute file path");
    }

    // Check for memory addresses
    std::regex addr_pattern(patterns::MEMORY_ADDRESS);
    if (std::regex_search(msg, addr_pattern)) {
        findings.push_back("Memory address");
    }

    return findings;
}

// ============================================================================
// Mode Management
// ============================================================================

void ErrorSanitizer::setMode(SanitizationMode mode) {
    mode_ = mode;
}

SanitizationMode ErrorSanitizer::getMode() {
    return mode_;
}

void ErrorSanitizer::setProjectRoot(const std::string& root) {
    project_root_ = root;
}

// ============================================================================
// Helper Functions
// ============================================================================

std::string ErrorSanitizer::makePathRelative(const std::string& path) {
    if (project_root_.empty()) {
        return path;
    }

    if (path.find(project_root_) == 0) {
        return path.substr(project_root_.length());
    }

    return path;
}

std::string ErrorSanitizer::redactPattern(
    const std::string& text,
    const std::regex& pattern
) {
    return std::regex_replace(text, pattern, "<redacted>");
}

} // namespace error
} // namespace naab
