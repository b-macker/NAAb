#pragma once

#include <string>
#include <vector>
#include <optional>

namespace naab {
namespace error {

/**
 * SuggestionSystem: Provides "Did you mean?" suggestions for errors (Phase 4.1.5)
 *
 * Uses fuzzy matching (Levenshtein distance) to find similar identifiers
 * when a name is not found.
 */
class SuggestionSystem {
public:
    /**
     * Find the closest match to input from a list of candidates
     *
     * @param input The misspelled or unknown identifier
     * @param candidates List of valid identifiers in scope
     * @param max_distance Maximum Levenshtein distance (default: 2)
     * @return Closest match if within max_distance, nullopt otherwise
     */
    static std::optional<std::string> findClosestMatch(
        const std::string& input,
        const std::vector<std::string>& candidates,
        size_t max_distance = 2);

    /**
     * Generate suggestion message for an undefined variable
     *
     * @param undefined_name The variable that wasn't found
     * @param scope_variables Variables available in current scope
     * @return Suggestion string like "Did you mean 'count'?"
     */
    static std::string suggestVariable(
        const std::string& undefined_name,
        const std::vector<std::string>& scope_variables);

    /**
     * Generate suggestion for a type mismatch
     *
     * @param expected Expected type name
     * @param actual Actual type name
     * @return Suggestion string with conversion hint
     */
    static std::string suggestTypeConversion(
        const std::string& expected,
        const std::string& actual);

    /**
     * Generate suggestion for a missing module import
     *
     * @param module_name Module that might be needed
     * @return Suggestion string like "Add 'import math'"
     */
    static std::string suggestImport(const std::string& module_name);

private:
    /**
     * Calculate Levenshtein distance between two strings
     *
     * @param s1 First string
     * @param s2 Second string
     * @return Edit distance
     */
    static size_t levenshteinDistance(const std::string& s1, const std::string& s2);
};

} // namespace error
} // namespace naab

