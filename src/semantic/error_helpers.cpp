// NAAb Error Helpers - Fuzzy matching and "Did you mean?" suggestions
// Provides intelligent suggestions for common typos and mistakes

#include "naab/error_helpers.h"
#include <algorithm>
#include <vector>
#include <string>
#include <unordered_set>
#include <fmt/core.h>

namespace naab {
namespace error {

// ============================================================================
// Levenshtein Distance - String similarity metric
// ============================================================================

// Compute edit distance between two strings
size_t levenshteinDistance(const std::string& s1, const std::string& s2) {
    const size_t len1 = s1.size();
    const size_t len2 = s2.size();

    // Create distance matrix
    std::vector<std::vector<size_t>> dp(len1 + 1, std::vector<size_t>(len2 + 1));

    // Initialize base cases
    for (size_t i = 0; i <= len1; ++i) {
        dp[i][0] = i;
    }
    for (size_t j = 0; j <= len2; ++j) {
        dp[0][j] = j;
    }

    // Fill matrix
    for (size_t i = 1; i <= len1; ++i) {
        for (size_t j = 1; j <= len2; ++j) {
            if (s1[i - 1] == s2[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1];  // No change needed
            } else {
                dp[i][j] = 1 + std::min({
                    dp[i - 1][j],      // Deletion
                    dp[i][j - 1],      // Insertion
                    dp[i - 1][j - 1]   // Substitution
                });
            }
        }
    }

    return dp[len1][len2];
}

// ============================================================================
// Fuzzy Matching - Find similar strings
// ============================================================================

std::vector<std::string> findSimilarStrings(
    const std::string& target,
    const std::vector<std::string>& candidates,
    size_t max_distance) {

    std::vector<std::pair<std::string, size_t>> matches;

    // Find all candidates within max_distance
    for (const auto& candidate : candidates) {
        size_t distance = levenshteinDistance(target, candidate);
        if (distance <= max_distance) {
            matches.emplace_back(candidate, distance);
        }
    }

    // Sort by distance (closest first)
    std::sort(matches.begin(), matches.end(),
        [](const auto& a, const auto& b) {
            return a.second < b.second;
        });

    // Extract just the strings
    std::vector<std::string> result;
    for (const auto& match : matches) {
        result.push_back(match.first);
    }

    return result;
}

// ============================================================================
// Common Error Patterns
// ============================================================================

std::string suggestForUndefinedVariable(
    const std::string& var_name,
    const std::vector<std::string>& defined_vars) {

    // Check for empty input
    if (var_name.empty()) {
        return "";
    }

    // Check if it's a standard library module
    static const std::unordered_set<std::string> stdlib_modules = {
        "io", "json", "string", "array", "math", "file", "http",
        "time", "regex", "crypto", "csv", "env", "collections", "core"
    };

    if (stdlib_modules.count(var_name) > 0) {
        return fmt::format(
            "Help: '{}' is a standard library module. Did you forget to import it?\n"
            "  Add this at the top of your file:\n"
            "    use {}\n\n"
            "  Available stdlib modules: io, json, string, array, file, http, time, regex, crypto, ...",
            var_name, var_name
        );
    }

    if (defined_vars.empty()) {
        return "";
    }

    // Try fuzzy matching with distance <= 2
    auto similar = findSimilarStrings(var_name, defined_vars, 2);

    if (!similar.empty()) {
        if (similar.size() == 1) {
            return "Did you mean '" + similar[0] + "'?";
        } else if (similar.size() <= 3) {
            // Show up to 3 suggestions
            std::string suggestion = "Did you mean one of these? ";
            for (size_t i = 0; i < similar.size(); ++i) {
                if (i > 0) suggestion += ", ";
                suggestion += "'" + similar[i] + "'";
            }
            return suggestion;
        }
    }

    // Check for common typos
    if (var_name == "cout" || var_name == "printf") {
        return "Did you mean 'print()'?";
    }
    if (var_name == "len") {
        return "Did you mean 'length' or 'size'?";
    }
    if (var_name == "def") {
        return "Did you mean 'fn' (for functions)?";
    }

    return "";
}

std::string suggestForUndefinedFunction(
    const std::string& func_name,
    const std::vector<std::string>& defined_funcs) {

    // Try fuzzy matching
    auto similar = findSimilarStrings(func_name, defined_funcs, 2);

    if (!similar.empty()) {
        if (similar.size() == 1) {
            return "Did you mean '" + similar[0] + "'?";
        } else if (similar.size() <= 3) {
            std::string suggestion = "Did you mean one of these? ";
            for (size_t i = 0; i < similar.size(); ++i) {
                if (i > 0) suggestion += ", ";
                suggestion += "'" + similar[i] + "()'";
            }
            return suggestion;
        }
    }

    // Check for common built-in function typos
    if (func_name == "println" || func_name == "printf") {
        return "Did you mean 'print()'?";
    }

    return "";
}

std::string suggestForTypeMismatch(
    const std::string& expected,
    const std::string& actual) {

    if (expected == "int" && actual == "string") {
        return "Try converting with 'toInt()' or use an integer literal";
    }
    if (expected == "string" && actual == "int") {
        return "Try converting with 'toString()' or use a string literal";
    }
    if (expected == "bool" && (actual == "int" || actual == "string")) {
        return "Use a boolean expression like '== 0' or 'isEmpty()'";
    }

    return "";
}

// ============================================================================
// Case Sensitivity Helpers
// ============================================================================

std::string checkCaseSensitivity(
    const std::string& name,
    const std::vector<std::string>& candidates) {

    // Convert to lowercase for comparison
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

    for (const auto& candidate : candidates) {
        std::string lower_candidate = candidate;
        std::transform(lower_candidate.begin(), lower_candidate.end(),
                      lower_candidate.begin(), ::tolower);

        if (lower_name == lower_candidate && name != candidate) {
            return "Note: '" + candidate + "' exists but names are case-sensitive";
        }
    }

    return "";
}

// ============================================================================
// Keyword Suggestions
// ============================================================================

std::string suggestForKeywordTypo(const std::string& token) {
    static const std::vector<std::string> keywords = {
        "let", "fn", "if", "else", "for", "while", "return",
        "true", "false", "null", "use", "as", "main", "print"
    };

    auto similar = findSimilarStrings(token, keywords, 1);
    if (!similar.empty()) {
        return "Did you mean the keyword '" + similar[0] + "'?";
    }

    return "";
}

} // namespace error
} // namespace naab
