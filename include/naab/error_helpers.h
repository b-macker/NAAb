#ifndef NAAB_ERROR_HELPERS_H
#define NAAB_ERROR_HELPERS_H

// NAAb Error Helpers - Fuzzy matching and "Did you mean?" suggestions
// Provides intelligent suggestions for common typos and mistakes

#include <string>
#include <vector>

namespace naab {
namespace error {

// ============================================================================
// String Similarity
// ============================================================================

// Compute Levenshtein distance (edit distance) between two strings
size_t levenshteinDistance(const std::string& s1, const std::string& s2);

// Find strings similar to target within max_distance edits
// Returns matches sorted by similarity (closest first)
std::vector<std::string> findSimilarStrings(
    const std::string& target,
    const std::vector<std::string>& candidates,
    size_t max_distance = 2);

// ============================================================================
// Suggestion Generators
// ============================================================================

// Generate "Did you mean?" suggestion for undefined variable
// Returns empty string if no good suggestion found
std::string suggestForUndefinedVariable(
    const std::string& var_name,
    const std::vector<std::string>& defined_vars);

// Generate "Did you mean?" suggestion for undefined function
std::string suggestForUndefinedFunction(
    const std::string& func_name,
    const std::vector<std::string>& defined_funcs);

// Generate suggestion for type mismatch errors
std::string suggestForTypeMismatch(
    const std::string& expected,
    const std::string& actual);

// Check if similar name exists with different case
std::string checkCaseSensitivity(
    const std::string& name,
    const std::vector<std::string>& candidates);

// Suggest corrections for keyword typos
std::string suggestForKeywordTypo(const std::string& token);

} // namespace error
} // namespace naab

#endif // NAAB_ERROR_HELPERS_H
