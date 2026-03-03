#pragma once

#include "naab/analyzer/detection_types.h"
#include "naab/analyzer/lexical_detector.h"
#include "naab/analyzer/syntactic_analyzer.h"
#include "naab/analyzer/semantic_analyzer.h"
#include <string>
#include <vector>
#include <map>

namespace naab {
namespace analyzer {

/**
 * Language Score
 *
 * Score for a language for a specific task
 */
struct LanguageScore {
    std::string language;
    int score;                          // 0-100
    std::string reason;                 // Why this score
    std::vector<std::string> strengths; // What this language is good at
    std::vector<std::string> weaknesses; // What this language lacks
};

/**
 * Language Recommendation
 *
 * Comprehensive recommendation for language choice
 */
struct LanguageRecommendation {
    // Current state
    std::string current_language;
    int current_score;                  // 0-100

    // Optimal choice
    std::string optimal_language;
    int optimal_score;                  // 0-100
    int improvement_percent;            // How much better

    // Alternatives
    std::vector<LanguageScore> acceptable_languages;
    std::vector<std::string> avoid_languages;

    // Reasoning
    std::vector<std::string> reasons;   // Why optimal is better
    std::vector<std::string> tradeoffs; // What you lose/gain
};

/**
 * Language Scorer
 *
 * Scores languages for specific tasks using multi-factor analysis
 */
class LanguageScorer {
public:
    /**
     * Constructor with task→language matrix
     *
     * @param task_language_matrix Matrix from governance config
     */
    LanguageScorer(
        const std::map<std::string, std::map<std::string, int>>& task_language_matrix
    );

    /**
     * Score a language for a specific task
     *
     * @param language Language to score
     * @param task Task type
     * @return Language score
     */
    LanguageScore scoreLanguageForTask(
        const std::string& language,
        const std::string& task
    ) const;

    /**
     * Get recommendation for code
     *
     * @param current_lang Current language being used
     * @param semantic_analysis Semantic analysis of code
     * @param lexical_categories Lexical token counts
     * @param syntactic_profile Syntactic structure
     * @return Comprehensive recommendation
     */
    LanguageRecommendation getRecommendation(
        const std::string& current_lang,
        const SemanticAnalysis& semantic_analysis,
        const std::map<std::string, int>& lexical_categories,
        const SyntacticProfile& syntactic_profile
    ) const;

    /**
     * Rank all languages for a task
     *
     * @param task Task type
     * @return Languages sorted by score (descending)
     */
    std::vector<LanguageScore> rankLanguagesForTask(
        const std::string& task
    ) const;

private:
    // Task→Language score matrix (from govern.json)
    std::map<std::string, std::map<std::string, int>> task_language_matrix_;

    // Weights for multi-factor scoring
    static constexpr double LEXICAL_WEIGHT = 0.25;
    static constexpr double SYNTACTIC_WEIGHT = 0.20;
    static constexpr double SEMANTIC_WEIGHT = 0.40;
    static constexpr double PERFORMANCE_WEIGHT = 0.15;

    /**
     * Map TaskIntent to task category string
     */
    std::string taskIntentToCategory(TaskIntent intent) const;

    /**
     * Calculate composite score
     */
    int calculateCompositeScore(
        const std::string& language,
        const SemanticAnalysis& semantic,
        const std::map<std::string, int>& lexical,
        const SyntacticProfile& syntactic
    ) const;

    /**
     * Generate reasons for recommendation
     */
    std::vector<std::string> generateReasons(
        const std::string& current_lang,
        const std::string& optimal_lang,
        const SemanticAnalysis& semantic
    ) const;

    /**
     * Analyze tradeoffs between languages
     */
    std::vector<std::string> analyzeTradeoffs(
        const std::string& from_lang,
        const std::string& to_lang
    ) const;

    /**
     * Get language strengths
     */
    std::vector<std::string> getLanguageStrengths(
        const std::string& language,
        const std::string& task
    ) const;

    /**
     * Get language weaknesses
     */
    std::vector<std::string> getLanguageWeaknesses(
        const std::string& language,
        const std::string& task
    ) const;

    /**
     * Find optimal language for task
     */
    std::string findOptimalLanguage(
        const std::string& task,
        int& out_score
    ) const;
};

} // namespace analyzer
} // namespace naab
