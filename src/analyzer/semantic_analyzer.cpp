#include "naab/analyzer/semantic_analyzer.h"
#include <algorithm>
#include <numeric>

namespace naab {
namespace analyzer {

SemanticAnalyzer::SemanticAnalyzer() {}

SemanticAnalysis SemanticAnalyzer::analyze(
    const std::string& code,
    const std::map<std::string, int>& lexical_categories,
    const SyntacticProfile& syntactic_profile
) const {
    SemanticAnalysis analysis;

    // Infer primary and secondary intents
    analysis.primary_intent = inferPrimaryIntent(lexical_categories, syntactic_profile);
    analysis.secondary_intents = inferSecondaryIntents(lexical_categories);

    // Build computational profile
    analysis.computational_profile = buildComputationalProfile(
        analysis.primary_intent, syntactic_profile
    );

    // Detect data flow pattern
    analysis.data_flow = detectDataFlowPattern(syntactic_profile);

    // Infer performance criteria
    analysis.performance_criteria = inferPerformanceCriteria(
        analysis.primary_intent, analysis.computational_profile
    );

    // Calculate confidence
    analysis.confidence = calculateConfidence(lexical_categories, analysis.primary_intent);

    return analysis;
}

TaskIntent SemanticAnalyzer::inferPrimaryIntent(
    const std::map<std::string, int>& lexical_categories,
    const SyntacticProfile& syntactic_profile
) const {
    // Find lexical category with highest count
    auto max_elem = std::max_element(
        lexical_categories.begin(), lexical_categories.end(),
        [](const auto& a, const auto& b) {
            return a.second < b.second;
        }
    );

    if (max_elem == lexical_categories.end() || max_elem->second == 0) {
        return TaskIntent::UNKNOWN;
    }

    // Convert dominant lexical category to intent
    TaskIntent lexical_intent = categoryToIntent(max_elem->first);

    // Refine based on syntactic patterns

    // If heavy loops + numerical tokens → NUMERICAL_COMPUTATION
    if (lexical_intent == TaskIntent::NUMERICAL_COMPUTATION) {
        if (syntactic_profile.has_nested_loops || syntactic_profile.loop_count > 2) {
            return TaskIntent::NUMERICAL_COMPUTATION;
        }
        if (syntactic_profile.has_large_iterations) {
            return TaskIntent::PARALLEL_COMPUTATION;
        }
    }

    // If string operations + parsing imports → DATA_PARSING
    if (lexical_intent == TaskIntent::STRING_MANIPULATION) {
        for (const auto& module : syntactic_profile.imported_modules) {
            if (module == "json" || module == "xml" || module == "csv" ||
                module == "yaml" || module == "toml") {
                return TaskIntent::DATA_PARSING;
            }
        }
    }

    // If file operations + shell commands → CLI_TOOL
    if (lexical_intent == TaskIntent::FILE_OPERATIONS) {
        auto shell_it = lexical_categories.find("shell_commands");
        if (shell_it != lexical_categories.end() && shell_it->second > 3) {
            return TaskIntent::CLI_TOOL;
        }
    }

    // If network operations + high external calls → WEB_SERVICE
    if (lexical_intent == TaskIntent::NETWORK_COMMUNICATION) {
        if (syntactic_profile.external_call_count > 5) {
            return TaskIntent::WEB_SERVICE;
        }
    }

    // If concurrency patterns → ASYNC_OPERATIONS or PARALLEL_COMPUTATION
    if (lexical_intent == TaskIntent::ASYNC_OPERATIONS) {
        if (syntactic_profile.has_nested_loops) {
            return TaskIntent::PARALLEL_COMPUTATION;
        }
        return TaskIntent::ASYNC_OPERATIONS;
    }

    // If systems programming + memory management → MEMORY_MANAGEMENT
    if (lexical_intent == TaskIntent::SYSTEMS_PROGRAMMING) {
        if (syntactic_profile.allocates_memory || syntactic_profile.manages_lifetime) {
            return TaskIntent::MEMORY_MANAGEMENT;
        }
        if (syntactic_profile.uses_pointers) {
            return TaskIntent::SYSTEMS_PROGRAMMING;
        }
    }

    // If data structures + array operations → DATA_TRANSFORMATION
    auto ds_it = lexical_categories.find("data_structures");
    if (ds_it != lexical_categories.end() && ds_it->second > 3) {
        if (syntactic_profile.has_array_operations || syntactic_profile.has_pipeline) {
            return TaskIntent::DATA_TRANSFORMATION;
        }
    }

    return lexical_intent;
}

std::vector<TaskIntent> SemanticAnalyzer::inferSecondaryIntents(
    const std::map<std::string, int>& lexical_categories
) const {
    std::vector<TaskIntent> intents;

    // Collect all categories with significant token counts (> 2)
    for (const auto& [category, count] : lexical_categories) {
        if (count > 2) {
            TaskIntent intent = categoryToIntent(category);
            if (intent != TaskIntent::UNKNOWN) {
                intents.push_back(intent);
            }
        }
    }

    // Sort by token count (descending)
    std::sort(intents.begin(), intents.end());

    // Remove duplicates
    intents.erase(std::unique(intents.begin(), intents.end()), intents.end());

    return intents;
}

ComputationalProfile SemanticAnalyzer::buildComputationalProfile(
    const TaskIntent& primary_intent,
    const SyntacticProfile& syntactic_profile
) const {
    ComputationalProfile profile;

    // CPU intensive tasks
    switch (primary_intent) {
        case TaskIntent::NUMERICAL_COMPUTATION:
        case TaskIntent::STATISTICAL_ANALYSIS:
        case TaskIntent::LINEAR_ALGEBRA:
        case TaskIntent::SIGNAL_PROCESSING:
        case TaskIntent::MACHINE_LEARNING:
            profile.is_cpu_intensive = true;
            break;
        default:
            // Check for heavy loops as indicator
            if (syntactic_profile.has_nested_loops ||
                syntactic_profile.has_large_iterations ||
                syntactic_profile.loop_count > 5) {
                profile.is_cpu_intensive = true;
            }
            break;
    }

    // Memory intensive tasks
    if (primary_intent == TaskIntent::MEMORY_MANAGEMENT ||
        syntactic_profile.allocates_memory ||
        syntactic_profile.uses_pointers) {
        profile.is_memory_intensive = true;
    }

    // I/O intensive tasks
    switch (primary_intent) {
        case TaskIntent::FILE_OPERATIONS:
        case TaskIntent::NETWORK_COMMUNICATION:
        case TaskIntent::DATABASE_ACCESS:
        case TaskIntent::STREAM_PROCESSING:
            profile.is_io_intensive = true;
            break;
        default:
            break;
    }

    // Latency sensitive (real-time, interactive)
    if (primary_intent == TaskIntent::EVENT_HANDLING ||
        primary_intent == TaskIntent::WEB_SERVICE ||
        primary_intent == TaskIntent::ASYNC_OPERATIONS) {
        profile.is_latency_sensitive = true;
    }

    // Throughput focused (batch processing)
    if (primary_intent == TaskIntent::BATCH_PROCESSING ||
        primary_intent == TaskIntent::DATA_TRANSFORMATION ||
        primary_intent == TaskIntent::PARALLEL_COMPUTATION) {
        profile.is_throughput_focused = true;
    }

    return profile;
}

DataFlowPattern SemanticAnalyzer::detectDataFlowPattern(
    const SyntacticProfile& syntactic_profile
) const {
    DataFlowPattern pattern;

    // Pipeline: data flows linearly through stages
    if (syntactic_profile.has_pipeline) {
        pattern.is_pipeline = true;
    }

    // Map-reduce: functional transformation patterns
    if (syntactic_profile.has_array_operations) {
        pattern.is_map_reduce = true;
    }

    // Streaming: incremental processing
    if (syntactic_profile.external_call_count > 3) {
        pattern.is_streaming = true;
    }

    // Batch: process all data at once
    if (syntactic_profile.has_large_iterations) {
        pattern.is_batch = true;
    }

    // Scatter-gather: fork/join parallelism
    // Heuristic: multiple functions + potential parallelism
    if (syntactic_profile.function_count > 3 &&
        syntactic_profile.has_array_operations) {
        pattern.is_scatter_gather = true;
    }

    return pattern;
}

PerformanceCriteria SemanticAnalyzer::inferPerformanceCriteria(
    const TaskIntent& primary_intent,
    const ComputationalProfile& comp_profile
) const {
    PerformanceCriteria criteria;

    // CPU-intensive → execution speed matters most
    if (comp_profile.is_cpu_intensive) {
        criteria.priorities.push_back(PerformancePriority::EXECUTION_SPEED);
    }

    // Memory-intensive → memory usage matters
    if (comp_profile.is_memory_intensive) {
        criteria.priorities.push_back(PerformancePriority::MEMORY_USAGE);
    }

    // Latency-sensitive → startup time + execution speed
    if (comp_profile.is_latency_sensitive) {
        criteria.priorities.push_back(PerformancePriority::STARTUP_TIME);
        if (std::find(criteria.priorities.begin(), criteria.priorities.end(),
                     PerformancePriority::EXECUTION_SPEED) == criteria.priorities.end()) {
            criteria.priorities.push_back(PerformancePriority::EXECUTION_SPEED);
        }
    }

    // Simple scripts/tools → developer time
    switch (primary_intent) {
        case TaskIntent::CLI_TOOL:
        case TaskIntent::STRING_MANIPULATION:
        case TaskIntent::DATA_PARSING:
            criteria.priorities.push_back(PerformancePriority::DEVELOPER_TIME);
            break;
        default:
            break;
    }

    // Complex systems → maintainability
    if (primary_intent == TaskIntent::SYSTEMS_PROGRAMMING ||
        primary_intent == TaskIntent::DISTRIBUTED_COMPUTING) {
        criteria.priorities.push_back(PerformancePriority::MAINTAINABILITY);
    }

    // If no specific criteria identified, default to developer time
    if (criteria.priorities.empty()) {
        criteria.priorities.push_back(PerformancePriority::DEVELOPER_TIME);
    }

    return criteria;
}

double SemanticAnalyzer::calculateConfidence(
    const std::map<std::string, int>& lexical_categories,
    const TaskIntent& primary_intent
) const {
    if (primary_intent == TaskIntent::UNKNOWN) {
        return 0.0;
    }

    // Calculate total token count
    int total_tokens = 0;
    int max_tokens = 0;

    for (const auto& [category, count] : lexical_categories) {
        total_tokens += count;
        max_tokens = std::max(max_tokens, count);
    }

    if (total_tokens == 0) {
        return 0.0;
    }

    // Confidence based on dominant category strength
    double dominance_ratio = static_cast<double>(max_tokens) / total_tokens;

    // More tokens → higher confidence
    double token_confidence = std::min(1.0, total_tokens / 10.0);

    // Combine factors
    double confidence = (dominance_ratio * 0.7) + (token_confidence * 0.3);

    return std::min(1.0, confidence);
}

TaskIntent SemanticAnalyzer::categoryToIntent(const std::string& category) const {
    // Map lexical categories to task intents
    if (category == "numerical_computation") {
        return TaskIntent::NUMERICAL_COMPUTATION;
    }
    if (category == "string_manipulation") {
        return TaskIntent::STRING_MANIPULATION;
    }
    if (category == "file_operations") {
        return TaskIntent::FILE_OPERATIONS;
    }
    if (category == "network_communication") {
        return TaskIntent::NETWORK_COMMUNICATION;
    }
    if (category == "concurrency") {
        return TaskIntent::ASYNC_OPERATIONS;
    }
    if (category == "systems_programming") {
        return TaskIntent::SYSTEMS_PROGRAMMING;
    }
    if (category == "data_structures") {
        return TaskIntent::DATA_TRANSFORMATION;
    }
    if (category == "data_parsing") {
        return TaskIntent::DATA_PARSING;
    }
    if (category == "database_access") {
        return TaskIntent::DATABASE_ACCESS;
    }
    if (category == "shell_commands") {
        return TaskIntent::CLI_TOOL;
    }

    return TaskIntent::UNKNOWN;
}

} // namespace analyzer
} // namespace naab
