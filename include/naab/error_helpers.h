#pragma once

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

// Suggest similar dict key when dict.get() returns null.
// Uses both Levenshtein distance AND substring containment for broader matching.
// Returns empty string if no suggestion found.
std::string suggestDictKey(
    const std::string& requested_key,
    const std::vector<std::string>& actual_keys);

// Print the "[hint] dict.get(...) returned null" diagnostic for a miss with
// no default. Shared by both engines (three call sites). Capped per run: the
// hint exists for typos, but building a map from data -- counting files from
// git log -- misses on every new key, and one hint per distinct key buried
// real output under hundreds of lines. Thread-safe.
void hintDictGetMiss(
    const std::string& requested_key,
    const std::vector<std::string>& actual_keys);

} // namespace error
} // namespace naab

