// NAAb Suggestion System Implementation
// Fuzzy matching for "Did you mean?" error suggestions (Phase 4.1)

#include "naab/suggestion_system.h"
#include <algorithm>
#include <limits>

namespace naab {
namespace error {

size_t SuggestionSystem::levenshteinDistance(const std::string& s1, const std::string& s2) {
    const size_t len1 = s1.length();
    const size_t len2 = s2.length();

    // Create distance matrix
    std::vector<std::vector<size_t>> dist(len1 + 1, std::vector<size_t>(len2 + 1));

    // Initialize base cases
    for (size_t i = 0; i <= len1; ++i) {
        dist[i][0] = i;
    }
    for (size_t j = 0; j <= len2; ++j) {
        dist[0][j] = j;
    }

    // Fill matrix using dynamic programming
    for (size_t i = 1; i <= len1; ++i) {
        for (size_t j = 1; j <= len2; ++j) {
            size_t cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;

            dist[i][j] = std::min({
                dist[i - 1][j] + 1,      // deletion
                dist[i][j - 1] + 1,      // insertion
                dist[i - 1][j - 1] + cost // substitution
            });
        }
    }

    return dist[len1][len2];
}

std::optional<std::string> SuggestionSystem::findClosestMatch(
    const std::string& input,
    const std::vector<std::string>& candidates,
    size_t max_distance) {

    if (candidates.empty()) {
        return std::nullopt;
    }

    std::string best_match;
    size_t best_distance = std::numeric_limits<size_t>::max();

    for (const auto& candidate : candidates) {
        size_t distance = levenshteinDistance(input, candidate);

        if (distance < best_distance) {
            best_distance = distance;
            best_match = candidate;
        }
    }

    // Only return match if within threshold
    if (best_distance <= max_distance) {
        return best_match;
    }

    return std::nullopt;
}

std::string SuggestionSystem::suggestVariable(
    const std::string& undefined_name,
    const std::vector<std::string>& scope_variables) {

    auto match = findClosestMatch(undefined_name, scope_variables);

    if (match.has_value()) {
        return "Did you mean '" + match.value() + "'?";
    }

    return "Variable '" + undefined_name + "' not defined. Check spelling or initialize before use.";
}

std::string SuggestionSystem::suggestTypeConversion(
    const std::string& expected,
    const std::string& actual) {

    // Common type conversion suggestions
    if (expected == "int" && actual == "string") {
        return "Convert string to int using int() function";
    }
    if (expected == "string" && (actual == "int" || actual == "double")) {
        return "Convert number to string using string() function";
    }
    if (expected == "double" && actual == "int") {
        return "Int will be automatically converted to double";
    }
    if (expected == "bool" && (actual == "int" || actual == "string")) {
        return "Use explicit boolean conversion: value != 0 or value != \"\"";
    }

    return "Type '" + actual + "' cannot be used where '" + expected + "' is expected";
}

std::string SuggestionSystem::suggestImport(const std::string& module_name) {
    return "Add 'import " + module_name + "' at the top of your file";
}

} // namespace error
} // namespace naab
