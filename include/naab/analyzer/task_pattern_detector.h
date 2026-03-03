#pragma once

#include "naab/analyzer/lexical_detector.h"
#include "naab/analyzer/syntactic_analyzer.h"
#include "naab/analyzer/semantic_analyzer.h"
#include "naab/analyzer/language_mismatch_detector.h"
#include "naab/analyzer/language_scorer.h"
#include "naab/analyzer/detection_types.h"
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace naab {
namespace analyzer {

/**
 * Detection Result
 *
 * Comprehensive analysis result combining all layers
 */
struct DetectionResult {
    // Primary task identification
    TaskIntent primary_task;
    std::vector<TaskIntent> all_tasks;

    // Language recommendations
    std::string optimal_language;
    std::vector<std::string> acceptable_languages;
    std::vector<std::string> avoid_languages;

    // Scoring
    int current_language_score;  // 0-100
    int optimal_language_score;  // 0-100
    int improvement_percent;     // How much better optimal is

    // Reasoning
    std::vector<std::string> reasons;    // Why this recommendation
    std::vector<std::string> tradeoffs;  // What you lose/gain

    // Detected issues
    std::vector<LanguageMismatch> mismatches;  // Cross-language idiom problems

    // Analysis confidence
    double confidence;  // 0.0-1.0

    // Raw analysis data
    std::map<std::string, int> lexical_categories;
    SyntacticProfile syntactic_profile;
    SemanticAnalysis semantic_analysis;

    // Refactoring suggestion (optional)
    std::optional<std::string> suggested_code;
};

/**
 * Comprehensive Task Pattern Detector
 *
 * Integrates all analysis layers to provide complete recommendations
 */
class ComprehensiveTaskDetector {
public:
    /**
     * Constructor with task→language matrix
     *
     * @param task_language_matrix Matrix from governance config
     */
    ComprehensiveTaskDetector(
        const std::map<std::string, std::map<std::string, int>>& task_language_matrix
    );

    /**
     * Analyze code and provide recommendations
     *
     * @param code Source code to analyze
     * @param current_lang Language the code is written in
     * @return Comprehensive detection result
     */
    DetectionResult analyze(
        const std::string& code,
        const std::string& current_lang
    ) const;

    /**
     * Quick check: should this code use a different language?
     *
     * @param code Source code
     * @param current_lang Current language
     * @return true if different language recommended
     */
    bool shouldUseDifferentLanguage(
        const std::string& code,
        const std::string& current_lang
    ) const;

    /**
     * Get improvement summary
     *
     * @param code Source code
     * @param current_lang Current language
     * @return Human-readable improvement summary
     */
    std::string getImprovementSummary(
        const std::string& code,
        const std::string& current_lang
    ) const;

private:
    // Analysis components
    LexicalDetector lexical_detector_;
    SyntacticAnalyzer syntactic_analyzer_;
    SemanticAnalyzer semantic_analyzer_;
    LanguageMismatchDetector mismatch_detector_;
    LanguageScorer language_scorer_;

    /**
     * Run lexical analysis
     */
    std::map<std::string, int> analyzeLexical(const std::string& code) const;

    /**
     * Run syntactic analysis
     */
    SyntacticProfile analyzeSyntactic(const std::string& code) const;

    /**
     * Run semantic analysis
     */
    SemanticAnalysis analyzeSemantic(
        const std::string& code,
        const std::map<std::string, int>& lexical_categories,
        const SyntacticProfile& syntactic_profile
    ) const;

    /**
     * Detect language mismatches
     */
    std::vector<LanguageMismatch> detectMismatches(
        const std::string& code,
        const std::string& current_lang
    ) const;

    /**
     * Compute final recommendation
     */
    DetectionResult computeRecommendation(
        const std::string& code,
        const std::string& current_lang,
        const std::map<std::string, int>& lexical,
        const SyntacticProfile& syntactic,
        const SemanticAnalysis& semantic,
        const std::vector<LanguageMismatch>& mismatches
    ) const;

    /**
     * Generate example code in optimal language
     */
    std::optional<std::string> generateSuggestedCode(
        const std::string& original_code,
        const std::string& from_lang,
        const std::string& to_lang,
        TaskIntent task
    ) const;

    /**
     * Format reasons with markdown
     */
    std::vector<std::string> formatReasons(
        const std::vector<std::string>& reasons
    ) const;
};

/**
 * Task Pattern Detector Factory
 *
 * Creates detector with configuration from govern.json
 */
class TaskPatternDetectorFactory {
public:
    /**
     * Create detector from task→language matrix
     *
     * @param task_language_matrix Matrix from governance config
     * @return Configured detector
     */
    static ComprehensiveTaskDetector create(
        const std::map<std::string, std::map<std::string, int>>& task_language_matrix
    );

    /**
     * Create detector with default matrix
     *
     * @return Detector with built-in defaults
     */
    static ComprehensiveTaskDetector createDefault();

private:
    /**
     * Get default task→language matrix
     */
    static std::map<std::string, std::map<std::string, int>> getDefaultMatrix();
};

} // namespace analyzer
} // namespace naab
