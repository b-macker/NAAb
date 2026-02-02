//
// NAAb Language - Safe Regex Implementation
// Implements ReDoS protection with timeouts and pattern analysis
//

#include "naab/safe_regex.h"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace naab {
namespace regex_safety {

// Exception message formatters
std::string RegexTimeoutException::formatMessage(const std::string& pattern,
                                                 std::chrono::milliseconds timeout) {
    std::ostringstream oss;
    oss << "Regex operation timed out after " << timeout.count()
        << "ms. Pattern may cause catastrophic backtracking: "
        << (pattern.length() > 50 ? pattern.substr(0, 50) + "..." : pattern);
    return oss.str();
}

std::string RegexInputSizeException::formatMessage(size_t actual_size, size_t max_size) {
    std::ostringstream oss;
    oss << "Regex input size " << actual_size << " bytes exceeds maximum "
        << max_size << " bytes";
    return oss.str();
}

std::string RegexDangerousPatternException::formatMessage(const std::string& pattern,
                                                          const std::string& reason) {
    std::ostringstream oss;
    oss << "Potentially dangerous regex pattern detected: " << reason
        << ". Pattern: "
        << (pattern.length() > 50 ? pattern.substr(0, 50) + "..." : pattern);
    return oss.str();
}

// SafeRegex implementation

void SafeRegex::validateInputSize(const std::string& text) const {
    if (text.size() > limits_.max_input_size) {
        throw RegexInputSizeException(text.size(), limits_.max_input_size);
    }
}

void SafeRegex::validatePatternSize(const std::string& pattern) const {
    if (pattern.size() > limits_.max_pattern_length) {
        throw std::runtime_error(
            "Regex pattern length " + std::to_string(pattern.size()) +
            " exceeds maximum " + std::to_string(limits_.max_pattern_length));
    }
}

void SafeRegex::validatePattern(const std::string& pattern) const {
    validatePatternSize(pattern);

    if (limits_.strict_validation) {
        PatternComplexity complexity = analyzePattern(pattern);

        if (!complexity.is_safe) {
            throw RegexDangerousPatternException(pattern, complexity.warning);
        }

        // Warn if backtracking score is moderate but not blocking
        if (complexity.backtracking_score > 50 && complexity.backtracking_score < 100) {
            // Log warning but allow execution
            // In production, this would go to a proper logging system
        }
    }
}

std::chrono::milliseconds SafeRegex::getEffectiveTimeout(
    std::chrono::milliseconds timeout) const {
    if (timeout.count() > 0) {
        return timeout;
    }
    return limits_.max_execution_time;
}

PatternComplexity SafeRegex::analyzePattern(const std::string& pattern) const {
    PatternComplexity result;

    // Check for nested quantifiers (highly dangerous)
    if (pattern_analysis::hasNestedQuantifiers(pattern)) {
        result.is_safe = false;
        result.backtracking_score += 100;
        result.warning = "Pattern contains nested quantifiers (e.g., (a+)+), which can cause catastrophic backtracking";
        return result;
    }

    // Check for overlapping alternatives
    if (pattern_analysis::hasOverlappingAlternatives(pattern)) {
        result.backtracking_score += 50;
        result.warning = "Pattern may contain overlapping alternatives";
    }

    // Check for unbounded repetition
    if (pattern_analysis::hasUnboundedRepetition(pattern)) {
        result.backtracking_score += 30;
        if (result.warning.empty()) {
            result.warning = "Pattern contains unbounded repetition (e.g., .*)";
        }
    }

    // Estimate overall backtracking potential
    int backtrack_score = pattern_analysis::estimateBacktrackingScore(pattern);
    result.backtracking_score += backtrack_score;

    // Get nesting depth
    result.nesting_depth = pattern_analysis::getPatternNestingDepth(pattern);
    if (result.nesting_depth > 10) {
        result.backtracking_score += result.nesting_depth * 5;
        if (result.warning.empty()) {
            result.warning = "Pattern has deep nesting depth";
        }
    }

    // Count quantifiers
    result.quantifier_count = pattern_analysis::countQuantifiers(pattern);
    if (result.quantifier_count > 20) {
        result.backtracking_score += (result.quantifier_count - 20) * 2;
    }

    // Final safety determination
    result.is_safe = (result.backtracking_score < 100);

    return result;
}

template<typename Func>
auto SafeRegex::executeWithTimeout(Func&& func,
                                   std::chrono::milliseconds timeout,
                                   const std::string& operation_name)
    -> decltype(func()) {
    using ReturnType = decltype(func());

    // Launch async task
    auto future = std::async(std::launch::async, std::forward<Func>(func));

    // Wait with timeout
    if (future.wait_for(timeout) == std::future_status::timeout) {
        // Note: std::future doesn't have a way to cancel the task
        // The task will continue running in the background
        // In a production system, we'd use a more sophisticated approach
        throw RegexTimeoutException(operation_name, timeout);
    }

    return future.get();
}

bool SafeRegex::safeMatch(const std::string& text,
                         const std::string& pattern,
                         std::chrono::milliseconds timeout) {
    validateInputSize(text);
    validatePattern(pattern);

    auto effective_timeout = getEffectiveTimeout(timeout);

    auto operation = [&text, &pattern]() {
        std::regex re(pattern);
        return std::regex_match(text, re);
    };

    try {
        return executeWithTimeout(operation, effective_timeout, pattern);
    } catch (const std::regex_error& e) {
        throw std::runtime_error("Regex error: " + std::string(e.what()));
    }
}

bool SafeRegex::safeSearch(const std::string& text,
                           const std::string& pattern,
                           std::chrono::milliseconds timeout) {
    validateInputSize(text);
    validatePattern(pattern);

    auto effective_timeout = getEffectiveTimeout(timeout);

    auto operation = [&text, &pattern]() {
        std::regex re(pattern);
        return std::regex_search(text, re);
    };

    try {
        return executeWithTimeout(operation, effective_timeout, pattern);
    } catch (const std::regex_error& e) {
        throw std::runtime_error("Regex error: " + std::string(e.what()));
    }
}

bool SafeRegex::safeSearch(const std::string& text,
                           const std::string& pattern,
                           std::smatch& match,
                           std::chrono::milliseconds timeout) {
    validateInputSize(text);
    validatePattern(pattern);

    auto effective_timeout = getEffectiveTimeout(timeout);

    auto operation = [&text, &pattern, &match]() {
        std::regex re(pattern);
        return std::regex_search(text, match, re);
    };

    try {
        return executeWithTimeout(operation, effective_timeout, pattern);
    } catch (const std::regex_error& e) {
        throw std::runtime_error("Regex error: " + std::string(e.what()));
    }
}

std::string SafeRegex::safeReplace(const std::string& text,
                                   const std::string& pattern,
                                   const std::string& replacement,
                                   std::chrono::milliseconds timeout,
                                   bool replace_all) {
    validateInputSize(text);
    validatePattern(pattern);

    auto effective_timeout = getEffectiveTimeout(timeout);

    auto operation = [&text, &pattern, &replacement, replace_all]() {
        std::regex re(pattern);
        if (replace_all) {
            return std::regex_replace(text, re, replacement);
        } else {
            return std::regex_replace(text, re, replacement,
                                     std::regex_constants::format_first_only);
        }
    };

    try {
        return executeWithTimeout(operation, effective_timeout, pattern);
    } catch (const std::regex_error& e) {
        throw std::runtime_error("Regex error: " + std::string(e.what()));
    }
}

std::vector<std::string> SafeRegex::safeFindAll(const std::string& text,
                                               const std::string& pattern,
                                               std::chrono::milliseconds timeout) {
    validateInputSize(text);
    validatePattern(pattern);

    auto effective_timeout = getEffectiveTimeout(timeout);

    auto operation = [&text, &pattern, this]() {
        std::regex re(pattern);
        std::vector<std::string> matches;

        auto begin = std::sregex_iterator(text.begin(), text.end(), re);
        auto end = std::sregex_iterator();

        size_t count = 0;
        for (auto i = begin; i != end; ++i) {
            if (count >= limits_.max_matches) {
                throw std::runtime_error(
                    "Number of regex matches exceeds limit of " +
                    std::to_string(limits_.max_matches));
            }
            matches.push_back(i->str(0));
            ++count;
        }

        return matches;
    };

    try {
        return executeWithTimeout(operation, effective_timeout, pattern);
    } catch (const std::regex_error& e) {
        throw std::runtime_error("Regex error: " + std::string(e.what()));
    }
}

// Pattern analysis implementation
namespace pattern_analysis {

bool hasNestedQuantifiers(const std::string& pattern) {
    // Look for patterns like (a+)+ or (a*)* or (a+)* etc.
    // This is a simplified check - a full implementation would need a parser

    std::regex nested_quantifier_pattern(R"(\([^)]*[*+?][^)]*\)[*+?{])");
    try {
        return std::regex_search(pattern, nested_quantifier_pattern);
    } catch (...) {
        return false;
    }
}

bool hasOverlappingAlternatives(const std::string& pattern) {
    // Simple heuristic: look for alternatives where one is a prefix of another
    // Full detection would require parsing the regex AST

    size_t pos = pattern.find('|');
    if (pos == std::string::npos) {
        return false;
    }

    // This is a simplified check
    // A proper implementation would parse alternatives and check for overlaps
    return pattern.find("(") != std::string::npos && pattern.find("|") != std::string::npos;
}

bool hasUnboundedRepetition(const std::string& pattern) {
    // Check for .* or .+
    if (pattern.find(".*") != std::string::npos ||
        pattern.find(".+") != std::string::npos) {
        return true;
    }

    // Check for [...]* or [...]+
    std::regex unbounded_pattern(R"(\[[^\]]+\][*+])");
    try {
        return std::regex_search(pattern, unbounded_pattern);
    } catch (...) {
        return false;
    }
}

int estimateBacktrackingScore(const std::string& pattern) {
    int score = 0;

    // Count quantifiers
    for (char c : pattern) {
        if (c == '*' || c == '+' || c == '?') {
            score += 5;
        }
    }

    // Count alternations
    for (char c : pattern) {
        if (c == '|') {
            score += 10;
        }
    }

    // Count character classes
    size_t pos = 0;
    while ((pos = pattern.find('[', pos)) != std::string::npos) {
        score += 2;
        pos++;
    }

    return score;
}

int getPatternNestingDepth(const std::string& pattern) {
    int max_depth = 0;
    int current_depth = 0;

    for (char c : pattern) {
        if (c == '(' || c == '[') {
            current_depth++;
            max_depth = std::max(max_depth, current_depth);
        } else if (c == ')' || c == ']') {
            current_depth = std::max(0, current_depth - 1);
        }
    }

    return max_depth;
}

int countQuantifiers(const std::string& pattern) {
    int count = 0;

    for (size_t i = 0; i < pattern.length(); ++i) {
        char c = pattern[i];
        if (c == '*' || c == '+' || c == '?') {
            count++;
        } else if (c == '{') {
            // Count {n,m} style quantifiers
            size_t close = pattern.find('}', i);
            if (close != std::string::npos) {
                count++;
                i = close;
            }
        }
    }

    return count;
}

} // namespace pattern_analysis

// Global instance
SafeRegex& getGlobalSafeRegex() {
    static SafeRegex instance;
    return instance;
}

} // namespace regex_safety
} // namespace naab
