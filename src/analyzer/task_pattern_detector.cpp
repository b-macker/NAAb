#include "naab/analyzer/task_pattern_detector.h"
#include <sstream>
#include <algorithm>

namespace naab {
namespace analyzer {

ComprehensiveTaskDetector::ComprehensiveTaskDetector(
    const std::map<std::string, std::map<std::string, int>>& task_language_matrix
) : language_scorer_(task_language_matrix) {}

DetectionResult ComprehensiveTaskDetector::analyze(
    const std::string& code,
    const std::string& current_lang
) const {
    // Layer 1: Lexical analysis
    auto lexical = analyzeLexical(code);

    // Layer 2: Syntactic analysis
    auto syntactic = analyzeSyntactic(code);

    // Layer 3: Semantic analysis
    auto semantic = analyzeSemantic(code, lexical, syntactic);

    // Layer 4: Cross-language mismatch detection
    auto mismatches = detectMismatches(code, current_lang);

    // Layer 5: Composite scoring and recommendation
    auto result = computeRecommendation(
        code, current_lang, lexical, syntactic, semantic, mismatches
    );

    return result;
}

bool ComprehensiveTaskDetector::shouldUseDifferentLanguage(
    const std::string& code,
    const std::string& current_lang
) const {
    auto result = analyze(code, current_lang);

    // Recommend different language if:
    // 1. Improvement > 20%
    // 2. OR mismatches detected with confidence > 80
    // 3. OR current score < 60 and optimal score > 80

    if (result.improvement_percent > 20) {
        return true;
    }

    for (const auto& mismatch : result.mismatches) {
        if (mismatch.confidence > 80) {
            return true;
        }
    }

    if (result.current_language_score < 60 && result.optimal_language_score > 80) {
        return true;
    }

    return false;
}

std::string ComprehensiveTaskDetector::getImprovementSummary(
    const std::string& code,
    const std::string& current_lang
) const {
    auto result = analyze(code, current_lang);

    std::ostringstream summary;
    summary << "Language Optimization Analysis:\n\n";

    // Current state
    summary << "  Current: " << current_lang
            << " (score: " << result.current_language_score << "/100)\n";

    // Optimal recommendation
    summary << "  Optimal: " << result.optimal_language
            << " (score: " << result.optimal_language_score << "/100)\n";

    // Improvement
    if (result.improvement_percent > 0) {
        summary << "  Improvement: +" << result.improvement_percent << "%\n\n";
    } else if (current_lang == result.optimal_language) {
        summary << "  Status: Already using optimal language\n\n";
    } else {
        summary << "  Status: Current choice is acceptable\n\n";
    }

    // Top reasons
    if (!result.reasons.empty() && result.improvement_percent > 10) {
        summary << "  Top reasons to switch:\n";
        int count = 0;
        for (const auto& reason : result.reasons) {
            if (count++ >= 3) break;  // Show top 3
            summary << "    • " << reason << "\n";
        }
        summary << "\n";
    }

    // Detected issues
    if (!result.mismatches.empty()) {
        summary << "  Detected issues:\n";
        for (const auto& mismatch : result.mismatches) {
            if (mismatch.confidence > 70) {
                summary << "    ⚠ " << mismatch.pattern_detected << "\n";
                summary << "      " << mismatch.suggestion << "\n";
            }
        }
    }

    return summary.str();
}

std::map<std::string, int> ComprehensiveTaskDetector::analyzeLexical(
    const std::string& code
) const {
    return lexical_detector_.analyze(code);
}

SyntacticProfile ComprehensiveTaskDetector::analyzeSyntactic(
    const std::string& code
) const {
    return syntactic_analyzer_.analyze(code);
}

SemanticAnalysis ComprehensiveTaskDetector::analyzeSemantic(
    const std::string& code,
    const std::map<std::string, int>& lexical_categories,
    const SyntacticProfile& syntactic_profile
) const {
    return semantic_analyzer_.analyze(code, lexical_categories, syntactic_profile);
}

std::vector<LanguageMismatch> ComprehensiveTaskDetector::detectMismatches(
    const std::string& code,
    const std::string& current_lang
) const {
    return mismatch_detector_.detect(code, current_lang);
}

DetectionResult ComprehensiveTaskDetector::computeRecommendation(
    const std::string& code,
    const std::string& current_lang,
    const std::map<std::string, int>& lexical,
    const SyntacticProfile& syntactic,
    const SemanticAnalysis& semantic,
    const std::vector<LanguageMismatch>& mismatches
) const {
    DetectionResult result;

    // Store raw analysis data
    result.lexical_categories = lexical;
    result.syntactic_profile = syntactic;
    result.semantic_analysis = semantic;
    result.mismatches = mismatches;

    // Primary task
    result.primary_task = semantic.primary_intent;
    result.all_tasks = semantic.secondary_intents;
    result.all_tasks.insert(result.all_tasks.begin(), semantic.primary_intent);

    // Get language recommendation
    auto recommendation = language_scorer_.getRecommendation(
        current_lang, semantic, lexical, syntactic
    );

    result.optimal_language = recommendation.optimal_language;
    result.current_language_score = recommendation.current_score;
    result.optimal_language_score = recommendation.optimal_score;
    result.improvement_percent = recommendation.improvement_percent;

    // Extract acceptable languages
    for (const auto& lang_score : recommendation.acceptable_languages) {
        result.acceptable_languages.push_back(lang_score.language);
    }
    result.avoid_languages = recommendation.avoid_languages;

    // Combine reasons from scorer and mismatches
    result.reasons = formatReasons(recommendation.reasons);
    result.tradeoffs = recommendation.tradeoffs;

    // Add mismatch warnings to reasons
    for (const auto& mismatch : mismatches) {
        if (mismatch.confidence > 75) {
            std::string warning = "Detected " + mismatch.source_lang +
                                  " idiom: " + mismatch.pattern_detected;
            result.reasons.push_back(warning);
        }
    }

    // Overall confidence (weighted average)
    result.confidence = semantic.confidence * 0.7 +
                        (mismatches.empty() ? 0.3 : 0.1);

    // Generate suggested code if improvement is significant
    if (result.improvement_percent > 30 &&
        current_lang != result.optimal_language) {
        result.suggested_code = generateSuggestedCode(
            code, current_lang, result.optimal_language, result.primary_task
        );
    }

    return result;
}

std::optional<std::string> ComprehensiveTaskDetector::generateSuggestedCode(
    const std::string& original_code,
    const std::string& from_lang,
    const std::string& to_lang,
    TaskIntent task
) const {
    // Generate simple transformation examples
    std::ostringstream suggestion;

    suggestion << "// Suggested " << to_lang << " equivalent:\n";

    // Task-specific templates
    switch (task) {
        case TaskIntent::NUMERICAL_COMPUTATION:
            if (to_lang == "julia") {
                suggestion << "using LinearAlgebra, Statistics\n\n";
                suggestion << "# Fast numerical operations\n";
                suggestion << "data = randn(1000, 1000)\n";
                suggestion << "result = mean(data)\n";
            } else if (to_lang == "nim") {
                suggestion << "import math, sequtils\n\n";
                suggestion << "# Compiled performance\n";
                suggestion << "let data = @[1.0, 2.0, 3.0]\n";
                suggestion << "let result = data.sum() / float(data.len)\n";
            }
            break;

        case TaskIntent::STRING_MANIPULATION:
            if (to_lang == "python") {
                suggestion << "import re\n\n";
                suggestion << "# Rich string API\n";
                suggestion << "text = \"hello world\"\n";
                suggestion << "result = text.upper().split()\n";
            } else if (to_lang == "ruby") {
                suggestion << "# Expressive string handling\n";
                suggestion << "text = \"hello world\"\n";
                suggestion << "result = text.upcase.split\n";
            }
            break;

        case TaskIntent::FILE_OPERATIONS:
            if (to_lang == "shell" || to_lang == "bash") {
                suggestion << "#!/bin/bash\n\n";
                suggestion << "# Concise file operations\n";
                suggestion << "find . -name \"*.txt\" | xargs grep \"pattern\"\n";
            }
            break;

        case TaskIntent::WEB_SERVICE:
            if (to_lang == "javascript") {
                suggestion << "// Native fetch API\n";
                suggestion << "const response = await fetch(url);\n";
                suggestion << "const data = await response.json();\n";
            } else if (to_lang == "go") {
                suggestion << "package main\n\n";
                suggestion << "import \"net/http\"\n\n";
                suggestion << "// Production-grade HTTP\n";
                suggestion << "resp, err := http.Get(url)\n";
                suggestion << "defer resp.Body.Close()\n";
            }
            break;

        case TaskIntent::SYSTEMS_PROGRAMMING:
            if (to_lang == "zig") {
                suggestion << "const std = @import(\"std\");\n\n";
                suggestion << "// Explicit memory control\n";
                suggestion << "var allocator = std.heap.page_allocator;\n";
                suggestion << "const memory = try allocator.alloc(u8, 100);\n";
                suggestion << "defer allocator.free(memory);\n";
            } else if (to_lang == "rust") {
                suggestion << "// Memory-safe systems code\n";
                suggestion << "let data = vec![1, 2, 3];\n";
                suggestion << "let ptr = data.as_ptr();\n";
            }
            break;

        case TaskIntent::ASYNC_OPERATIONS:
            if (to_lang == "go") {
                suggestion << "package main\n\n";
                suggestion << "// Goroutines\n";
                suggestion << "go func() {\n";
                suggestion << "    // async work\n";
                suggestion << "}()\n";
            }
            break;

        default:
            // Generic example
            suggestion << "# Convert " << from_lang << " code to " << to_lang << "\n";
            suggestion << "# (task-specific example not available)\n";
            break;
    }

    return suggestion.str();
}

std::vector<std::string> ComprehensiveTaskDetector::formatReasons(
    const std::vector<std::string>& reasons
) const {
    // Reasons are already formatted, just return them
    return reasons;
}

// Factory implementation

ComprehensiveTaskDetector TaskPatternDetectorFactory::create(
    const std::map<std::string, std::map<std::string, int>>& task_language_matrix
) {
    return ComprehensiveTaskDetector(task_language_matrix);
}

ComprehensiveTaskDetector TaskPatternDetectorFactory::createDefault() {
    return ComprehensiveTaskDetector(getDefaultMatrix());
}

std::map<std::string, std::map<std::string, int>>
TaskPatternDetectorFactory::getDefaultMatrix() {
    std::map<std::string, std::map<std::string, int>> matrix;

    // Numerical operations
    matrix["numerical_operations"]["julia"] = 100;
    matrix["numerical_operations"]["nim"] = 95;
    matrix["numerical_operations"]["python"] = 70;
    matrix["numerical_operations"]["rust"] = 65;
    matrix["numerical_operations"]["go"] = 50;
    matrix["numerical_operations"]["javascript"] = 30;
    matrix["numerical_operations"]["shell"] = 10;

    // String processing
    matrix["string_processing"]["python"] = 100;
    matrix["string_processing"]["ruby"] = 95;
    matrix["string_processing"]["nim"] = 90;
    matrix["string_processing"]["javascript"] = 85;
    matrix["string_processing"]["go"] = 70;
    matrix["string_processing"]["rust"] = 60;
    matrix["string_processing"]["zig"] = 40;
    matrix["string_processing"]["cpp"] = 35;

    // File operations
    matrix["file_operations"]["shell"] = 100;
    matrix["file_operations"]["bash"] = 100;
    matrix["file_operations"]["python"] = 80;
    matrix["file_operations"]["nim"] = 75;
    matrix["file_operations"]["go"] = 70;
    matrix["file_operations"]["ruby"] = 70;
    matrix["file_operations"]["rust"] = 65;
    matrix["file_operations"]["javascript"] = 40;

    // Systems operations
    matrix["systems_operations"]["zig"] = 100;
    matrix["systems_operations"]["rust"] = 95;
    matrix["systems_operations"]["cpp"] = 85;
    matrix["systems_operations"]["nim"] = 80;
    matrix["systems_operations"]["go"] = 60;
    matrix["systems_operations"]["python"] = 20;
    matrix["systems_operations"]["javascript"] = 10;

    // Web APIs
    matrix["web_apis"]["javascript"] = 100;
    matrix["web_apis"]["python"] = 90;
    matrix["web_apis"]["go"] = 85;
    matrix["web_apis"]["ruby"] = 80;
    matrix["web_apis"]["nim"] = 70;
    matrix["web_apis"]["rust"] = 65;
    matrix["web_apis"]["zig"] = 40;
    matrix["web_apis"]["cpp"] = 35;

    // Concurrency
    matrix["concurrency"]["go"] = 100;
    matrix["concurrency"]["rust"] = 95;
    matrix["concurrency"]["julia"] = 85;
    matrix["concurrency"]["nim"] = 75;
    matrix["concurrency"]["python"] = 50;
    matrix["concurrency"]["javascript"] = 70;
    matrix["concurrency"]["ruby"] = 40;
    matrix["concurrency"]["shell"] = 60;

    // JSON processing
    matrix["json_processing"]["javascript"] = 100;
    matrix["json_processing"]["python"] = 95;
    matrix["json_processing"]["go"] = 90;
    matrix["json_processing"]["nim"] = 85;
    matrix["json_processing"]["ruby"] = 80;
    matrix["json_processing"]["rust"] = 75;
    matrix["json_processing"]["julia"] = 70;
    matrix["json_processing"]["cpp"] = 50;
    matrix["json_processing"]["shell"] = 30;

    // CLI tools
    matrix["cli_tools"]["shell"] = 100;
    matrix["cli_tools"]["bash"] = 100;
    matrix["cli_tools"]["python"] = 85;
    matrix["cli_tools"]["go"] = 80;
    matrix["cli_tools"]["rust"] = 75;
    matrix["cli_tools"]["nim"] = 70;
    matrix["cli_tools"]["ruby"] = 65;
    matrix["cli_tools"]["javascript"] = 50;

    return matrix;
}

} // namespace analyzer
} // namespace naab
