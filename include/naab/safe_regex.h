//
// NAAb Language - Safe Regex with ReDoS Protection
// Protects against Regular Expression Denial of Service attacks
//

#pragma once

#include <regex>
#include <string>
#include <chrono>
#include <future>
#include <stdexcept>
#include <functional>

namespace naab {
namespace regex_safety {

// Configuration for regex safety limits
struct RegexLimits {
    // Maximum time a regex operation can run before being aborted
    std::chrono::milliseconds max_execution_time{1000};  // 1 second default

    // Maximum input string size for regex operations
    size_t max_input_size{100000};  // 100KB default

    // Maximum pattern length
    size_t max_pattern_length{1000};  // 1KB default

    // Maximum number of matches to return
    size_t max_matches{10000};  // 10k matches default

    // Enable strict pattern validation
    bool strict_validation{true};
};

// Exception thrown when regex operation times out
class RegexTimeoutException : public std::runtime_error {
public:
    explicit RegexTimeoutException(const std::string& pattern,
                                   std::chrono::milliseconds timeout)
        : std::runtime_error(formatMessage(pattern, timeout)) {}

private:
    static std::string formatMessage(const std::string& pattern,
                                     std::chrono::milliseconds timeout);
};

// Exception thrown when regex input is too large
class RegexInputSizeException : public std::runtime_error {
public:
    explicit RegexInputSizeException(size_t actual_size, size_t max_size)
        : std::runtime_error(formatMessage(actual_size, max_size)) {}

private:
    static std::string formatMessage(size_t actual_size, size_t max_size);
};

// Exception thrown when regex pattern is potentially dangerous
class RegexDangerousPatternException : public std::runtime_error {
public:
    explicit RegexDangerousPatternException(const std::string& pattern,
                                           const std::string& reason)
        : std::runtime_error(formatMessage(pattern, reason)) {}

private:
    static std::string formatMessage(const std::string& pattern,
                                     const std::string& reason);
};

// Pattern complexity analysis result
struct PatternComplexity {
    bool is_safe{true};
    int backtracking_score{0};  // Higher = more dangerous
    int nesting_depth{0};
    int quantifier_count{0};
    std::string warning;
};

// Safe regex wrapper with timeout and complexity checking
class SafeRegex {
public:
    // Constructor with custom limits
    explicit SafeRegex(const RegexLimits& limits = RegexLimits())
        : limits_(limits) {}

    // Validate pattern and check for ReDoS vulnerabilities
    PatternComplexity analyzePattern(const std::string& pattern) const;

    // Safe regex_match with timeout
    bool safeMatch(const std::string& text,
                   const std::string& pattern,
                   std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

    // Safe regex_search with timeout
    bool safeSearch(const std::string& text,
                    const std::string& pattern,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

    // Safe regex_search with match results and timeout
    bool safeSearch(const std::string& text,
                    const std::string& pattern,
                    std::smatch& match,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

    // Safe regex_replace with timeout
    std::string safeReplace(const std::string& text,
                           const std::string& pattern,
                           const std::string& replacement,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(0),
                           bool replace_all = true);

    // Safe find all matches with timeout and match limit
    std::vector<std::string> safeFindAll(const std::string& text,
                                        const std::string& pattern,
                                        std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

    // Get current limits
    const RegexLimits& getLimits() const { return limits_; }

    // Set limits
    void setLimits(const RegexLimits& limits) { limits_ = limits; }

private:
    RegexLimits limits_;

    // Validate inputs
    void validateInputSize(const std::string& text) const;
    void validatePatternSize(const std::string& pattern) const;
    void validatePattern(const std::string& pattern) const;

    // Get effective timeout (use provided or default from limits)
    std::chrono::milliseconds getEffectiveTimeout(std::chrono::milliseconds timeout) const;

    // Execute regex operation with timeout using std::future
    template<typename Func>
    auto executeWithTimeout(Func&& func,
                           std::chrono::milliseconds timeout,
                           const std::string& operation_name)
        -> decltype(func());
};

// Pattern analysis utilities
namespace pattern_analysis {

// Check if pattern contains nested quantifiers (e.g., (a+)+ or (a*)+)
bool hasNestedQuantifiers(const std::string& pattern);

// Check if pattern has overlapping alternatives (e.g., (a|ab))
bool hasOverlappingAlternatives(const std::string& pattern);

// Check if pattern has unbounded repetition (e.g., .* or .+)
bool hasUnboundedRepetition(const std::string& pattern);

// Count backtracking potential
int estimateBacktrackingScore(const std::string& pattern);

// Get nesting depth of pattern
int getPatternNestingDepth(const std::string& pattern);

// Count quantifiers in pattern
int countQuantifiers(const std::string& pattern);

} // namespace pattern_analysis

// Global safe regex instance with default limits
SafeRegex& getGlobalSafeRegex();

} // namespace regex_safety
} // namespace naab
