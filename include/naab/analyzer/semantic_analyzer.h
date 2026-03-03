#pragma once

#include "naab/analyzer/detection_types.h"
#include "naab/analyzer/lexical_detector.h"
#include "naab/analyzer/syntactic_analyzer.h"
#include <string>
#include <vector>
#include <map>

namespace naab {
namespace analyzer {

/**
 * Semantic Analysis Result
 *
 * High-level understanding of code purpose
 */
struct SemanticAnalysis {
    TaskIntent primary_intent = TaskIntent::UNKNOWN;
    std::vector<TaskIntent> secondary_intents;

    ComputationalProfile computational_profile;
    DataFlowPattern data_flow;
    PerformanceCriteria performance_criteria;

    // Confidence in analysis (0.0-1.0)
    double confidence = 0.0;
};

/**
 * Semantic Layer - Intent Inference
 *
 * Infers programmer's intent by combining lexical and syntactic analysis
 */
class SemanticAnalyzer {
public:
    SemanticAnalyzer();

    /**
     * Analyze code semantics
     *
     * @param code Source code
     * @param lexical_categories Categories from lexical analysis
     * @param syntactic_profile Profile from syntactic analysis
     * @return Semantic analysis result
     */
    SemanticAnalysis analyze(
        const std::string& code,
        const std::map<std::string, int>& lexical_categories,
        const SyntacticProfile& syntactic_profile
    ) const;

private:
    /**
     * Infer primary task intent
     */
    TaskIntent inferPrimaryIntent(
        const std::map<std::string, int>& lexical_categories,
        const SyntacticProfile& syntactic_profile
    ) const;

    /**
     * Infer secondary intents
     */
    std::vector<TaskIntent> inferSecondaryIntents(
        const std::map<std::string, int>& lexical_categories
    ) const;

    /**
     * Build computational profile
     */
    ComputationalProfile buildComputationalProfile(
        const TaskIntent& primary_intent,
        const SyntacticProfile& syntactic_profile
    ) const;

    /**
     * Detect data flow pattern
     */
    DataFlowPattern detectDataFlowPattern(
        const SyntacticProfile& syntactic_profile
    ) const;

    /**
     * Infer performance criteria
     */
    PerformanceCriteria inferPerformanceCriteria(
        const TaskIntent& primary_intent,
        const ComputationalProfile& comp_profile
    ) const;

    /**
     * Calculate confidence score
     */
    double calculateConfidence(
        const std::map<std::string, int>& lexical_categories,
        const TaskIntent& primary_intent
    ) const;

    /**
     * Map lexical category to task intent
     */
    TaskIntent categoryToIntent(const std::string& category) const;
};

} // namespace analyzer
} // namespace naab
