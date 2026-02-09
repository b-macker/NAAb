#include "naab/utils/string_utils.h"
#include <algorithm>
#include <sstream>

namespace naab {
namespace utils {

int levenshteinDistance(const std::string& s1, const std::string& s2) {
    const size_t len1 = s1.size();
    const size_t len2 = s2.size();

    // Create distance matrix
    std::vector<std::vector<int>> d(len1 + 1, std::vector<int>(len2 + 1));

    // Initialize first column and row
    for (size_t i = 0; i <= len1; ++i) {
        d[i][0] = static_cast<int>(i);
    }
    for (size_t j = 0; j <= len2; ++j) {
        d[0][j] = static_cast<int>(j);
    }

    // Calculate distances
    for (size_t i = 1; i <= len1; ++i) {
        for (size_t j = 1; j <= len2; ++j) {
            int cost = (s1[i - 1] == s2[j - 1]) ? 0 : 1;
            d[i][j] = std::min({
                d[i - 1][j] + 1,      // deletion
                d[i][j - 1] + 1,      // insertion
                d[i - 1][j - 1] + cost // substitution
            });
        }
    }

    return d[len1][len2];
}

std::vector<std::string> findSimilar(const std::string& target,
                                     const std::vector<std::string>& candidates,
                                     int max_distance) {
    std::vector<std::pair<std::string, int>> similar_with_distance;

    for (const auto& candidate : candidates) {
        int distance = levenshteinDistance(target, candidate);
        if (distance <= max_distance) {
            similar_with_distance.emplace_back(candidate, distance);
        }
    }

    // Sort by distance (closest first)
    std::sort(similar_with_distance.begin(), similar_with_distance.end(),
              [](const auto& a, const auto& b) {
                  return a.second < b.second;
              });

    // Extract just the names
    std::vector<std::string> similar;
    for (const auto& pair : similar_with_distance) {
        similar.push_back(pair.first);
    }

    return similar;
}

std::string formatSuggestions(const std::string& wrong_name,
                              const std::vector<std::string>& similar) {
    if (similar.empty()) {
        return "";
    }

    std::ostringstream oss;
    oss << "\n  Did you mean: ";

    // Show up to 3 suggestions
    for (size_t i = 0; i < similar.size() && i < 3; ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << similar[i];
    }

    oss << "?";
    return oss.str();
}

} // namespace utils
} // namespace naab
