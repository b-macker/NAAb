#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>

namespace naab {
namespace analyzer {

/**
 * Lexical Layer - Token-based Pattern Detection
 *
 * Analyzes code at the token level to identify task categories.
 * Uses 200+ keywords and patterns across 10 token categories.
 */
class LexicalDetector {
public:
    LexicalDetector();

    /**
     * Analyze code and count token occurrences
     *
     * @param code Source code to analyze
     * @return Map of category → occurrence count
     */
    std::map<std::string, int> analyze(const std::string& code) const;

    /**
     * Get dominant token category
     *
     * @param code Source code
     * @return Category name with highest occurrence
     */
    std::string getDominantCategory(const std::string& code) const;

    /**
     * Get all detected categories with counts
     *
     * @param code Source code
     * @return Vector of (category, count) pairs, sorted by count descending
     */
    std::vector<std::pair<std::string, int>> getRankedCategories(const std::string& code) const;

private:
    // Token pattern databases (10 categories)
    std::vector<std::string> numeric_tokens_;
    std::vector<std::string> string_tokens_;
    std::vector<std::string> file_io_tokens_;
    std::vector<std::string> network_tokens_;
    std::vector<std::string> concurrency_tokens_;
    std::vector<std::string> systems_tokens_;
    std::vector<std::string> datastructure_tokens_;
    std::vector<std::string> parsing_tokens_;
    std::vector<std::string> database_tokens_;
    std::vector<std::string> shell_tokens_;

    /**
     * Count occurrences of tokens in code
     *
     * @param code Source code
     * @param tokens Token list
     * @return Number of matches
     */
    int countTokens(const std::string& code, const std::vector<std::string>& tokens) const;

    /**
     * Initialize token databases
     */
    void initializeTokens();
};

} // namespace analyzer
} // namespace naab
