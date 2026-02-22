#pragma once

#include <string>
#include <vector>

namespace naab {
namespace utils {

// Calculate Levenshtein distance between two strings
int levenshteinDistance(const std::string& s1, const std::string& s2);

// Find similar strings based on edit distance
std::vector<std::string> findSimilar(const std::string& target,
                                     const std::vector<std::string>& candidates,
                                     int max_distance = 2);

// Format suggestions for error messages
std::string formatSuggestions(const std::string& wrong_name,
                              const std::vector<std::string>& similar);

} // namespace utils
} // namespace naab

