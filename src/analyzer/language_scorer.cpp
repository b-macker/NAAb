#include "naab/analyzer/language_scorer.h"
#include <algorithm>
#include <numeric>
#include <cmath>

namespace naab {
namespace analyzer {

LanguageScorer::LanguageScorer(
    const std::map<std::string, std::map<std::string, int>>& task_language_matrix
) : task_language_matrix_(task_language_matrix) {}

LanguageScore LanguageScorer::scoreLanguageForTask(
    const std::string& language,
    const std::string& task
) const {
    LanguageScore score;
    score.language = language;
    score.score = 50; // Default neutral score

    // Look up in task→language matrix
    auto task_it = task_language_matrix_.find(task);
    if (task_it != task_language_matrix_.end()) {
        auto lang_it = task_it->second.find(language);
        if (lang_it != task_it->second.end()) {
            score.score = lang_it->second;
        }
    }

    // Generate reason based on score
    if (score.score >= 90) {
        score.reason = "Excellent choice for this task";
    } else if (score.score >= 75) {
        score.reason = "Very good for this task";
    } else if (score.score >= 60) {
        score.reason = "Acceptable choice";
    } else if (score.score >= 40) {
        score.reason = "Suboptimal but workable";
    } else {
        score.reason = "Poor choice for this task";
    }

    // Get strengths and weaknesses
    score.strengths = getLanguageStrengths(language, task);
    score.weaknesses = getLanguageWeaknesses(language, task);

    return score;
}

std::vector<LanguageScore> LanguageScorer::rankLanguagesForTask(
    const std::string& task
) const {
    std::vector<LanguageScore> rankings;

    // Get all languages for this task
    auto task_it = task_language_matrix_.find(task);
    if (task_it != task_language_matrix_.end()) {
        for (const auto& [language, score_value] : task_it->second) {
            rankings.push_back(scoreLanguageForTask(language, task));
        }
    }

    // Sort by score descending
    std::sort(rankings.begin(), rankings.end(),
        [](const LanguageScore& a, const LanguageScore& b) {
            return a.score > b.score;
        }
    );

    return rankings;
}

LanguageRecommendation LanguageScorer::getRecommendation(
    const std::string& current_lang,
    const SemanticAnalysis& semantic_analysis,
    const std::map<std::string, int>& lexical_categories,
    const SyntacticProfile& syntactic_profile
) const {
    LanguageRecommendation rec;
    rec.current_language = current_lang;

    // Map primary intent to task category
    std::string task_category = taskIntentToCategory(semantic_analysis.primary_intent);

    // Calculate current language score
    rec.current_score = calculateCompositeScore(
        current_lang, semantic_analysis, lexical_categories, syntactic_profile
    );

    // Find optimal language
    rec.optimal_language = findOptimalLanguage(task_category, rec.optimal_score);

    // Calculate improvement
    if (rec.current_score > 0) {
        rec.improvement_percent =
            ((rec.optimal_score - rec.current_score) * 100) / rec.current_score;
    } else {
        rec.improvement_percent = rec.optimal_score;
    }

    // Get alternative languages (score >= 70)
    auto all_rankings = rankLanguagesForTask(task_category);
    for (const auto& lang_score : all_rankings) {
        if (lang_score.score >= 70 && lang_score.language != rec.optimal_language) {
            rec.acceptable_languages.push_back(lang_score);
        } else if (lang_score.score < 40) {
            rec.avoid_languages.push_back(lang_score.language);
        }
    }

    // Generate reasons
    rec.reasons = generateReasons(current_lang, rec.optimal_language, semantic_analysis);

    // Analyze tradeoffs
    if (current_lang != rec.optimal_language) {
        rec.tradeoffs = analyzeTradeoffs(current_lang, rec.optimal_language);
    }

    return rec;
}

int LanguageScorer::calculateCompositeScore(
    const std::string& language,
    const SemanticAnalysis& semantic,
    const std::map<std::string, int>& lexical,
    const SyntacticProfile& syntactic
) const {
    // Get base score from task→language matrix
    std::string task_category = taskIntentToCategory(semantic.primary_intent);
    int base_score = scoreLanguageForTask(language, task_category).score;

    // Adjust based on computational profile
    double adjustment = 0.0;

    // CPU-intensive tasks favor compiled languages
    if (semantic.computational_profile.is_cpu_intensive) {
        if (language == "julia" || language == "nim" || language == "rust" ||
            language == "zig" || language == "cpp" || language == "go") {
            adjustment += 10.0;
        } else if (language == "python" || language == "ruby") {
            adjustment -= 10.0;
        }
    }

    // Memory-intensive tasks favor languages with manual control
    if (semantic.computational_profile.is_memory_intensive) {
        if (language == "zig" || language == "rust" || language == "cpp") {
            adjustment += 5.0;
        } else if (language == "python" || language == "javascript") {
            adjustment -= 5.0;
        }
    }

    // I/O-intensive tasks favor high-level languages
    if (semantic.computational_profile.is_io_intensive) {
        if (language == "python" || language == "javascript" || language == "go") {
            adjustment += 5.0;
        }
    }

    // Latency-sensitive tasks favor compiled languages with fast startup
    if (semantic.computational_profile.is_latency_sensitive) {
        if (language == "go" || language == "rust" || language == "zig") {
            adjustment += 8.0;
        } else if (language == "julia") {
            adjustment -= 5.0; // JIT warmup
        }
    }

    // Complex code prefers readable languages
    if (syntactic.complexity_score > 70) {
        if (language == "python" || language == "ruby" || language == "nim") {
            adjustment += 5.0;
        } else if (language == "cpp" || language == "rust") {
            adjustment -= 3.0;
        }
    }

    int final_score = base_score + static_cast<int>(adjustment);
    return std::max(0, std::min(100, final_score)); // Clamp to 0-100
}

std::string LanguageScorer::taskIntentToCategory(TaskIntent intent) const {
    switch (intent) {
        case TaskIntent::NUMERICAL_COMPUTATION:
        case TaskIntent::STATISTICAL_ANALYSIS:
        case TaskIntent::LINEAR_ALGEBRA:
        case TaskIntent::SIGNAL_PROCESSING:
        case TaskIntent::MACHINE_LEARNING:
            return "numerical_operations";

        case TaskIntent::STRING_MANIPULATION:
            return "string_processing";

        case TaskIntent::FILE_OPERATIONS:
            return "file_operations";

        case TaskIntent::NETWORK_COMMUNICATION:
        case TaskIntent::WEB_SERVICE:
            return "web_apis";

        case TaskIntent::SYSTEMS_PROGRAMMING:
        case TaskIntent::MEMORY_MANAGEMENT:
            return "systems_operations";

        case TaskIntent::DATA_PARSING:
        case TaskIntent::DATA_SERIALIZATION:
            return "json_processing";

        case TaskIntent::ASYNC_OPERATIONS:
        case TaskIntent::PARALLEL_COMPUTATION:
            return "concurrency";

        case TaskIntent::CLI_TOOL:
            return "cli_tools";

        default:
            return "general";
    }
}

std::vector<std::string> LanguageScorer::generateReasons(
    const std::string& current_lang,
    const std::string& optimal_lang,
    const SemanticAnalysis& semantic
) const {
    std::vector<std::string> reasons;

    if (current_lang == optimal_lang) {
        reasons.push_back("Current language is already optimal for this task");
        return reasons;
    }

    // Task-specific reasons
    switch (semantic.primary_intent) {
        case TaskIntent::NUMERICAL_COMPUTATION:
            if (optimal_lang == "julia") {
                reasons.push_back("Julia provides 10-100x speedup for numerical operations");
                reasons.push_back("Built-in BLAS/LAPACK integration");
                reasons.push_back("JIT compilation optimized for math");
            } else if (optimal_lang == "nim") {
                reasons.push_back("Nim compiles to fast native code");
                reasons.push_back("Good math library ecosystem");
            }
            break;

        case TaskIntent::STRING_MANIPULATION:
            if (optimal_lang == "python") {
                reasons.push_back("Python has rich string API and built-in regex");
                reasons.push_back("Extensive text processing libraries");
            } else if (optimal_lang == "ruby") {
                reasons.push_back("Ruby excels at string manipulation");
            }
            break;

        case TaskIntent::FILE_OPERATIONS:
            if (optimal_lang == "shell" || optimal_lang == "bash") {
                reasons.push_back("Shell tools (grep, find, sed) are purpose-built for file ops");
                reasons.push_back("Concise pipeline-based operations");
            }
            break;

        case TaskIntent::SYSTEMS_PROGRAMMING:
            if (optimal_lang == "zig") {
                reasons.push_back("Zig provides explicit memory control");
                reasons.push_back("Zero-cost abstractions and C interop");
            } else if (optimal_lang == "rust") {
                reasons.push_back("Rust ensures memory safety without GC overhead");
                reasons.push_back("Zero-cost abstractions");
            }
            break;

        case TaskIntent::WEB_SERVICE:
        case TaskIntent::NETWORK_COMMUNICATION:
            if (optimal_lang == "javascript") {
                reasons.push_back("JavaScript has native fetch API and JSON support");
            } else if (optimal_lang == "python") {
                reasons.push_back("Python's requests library is excellent");
            } else if (optimal_lang == "go") {
                reasons.push_back("Go's net/http package is production-grade");
            }
            break;

        case TaskIntent::ASYNC_OPERATIONS:
        case TaskIntent::PARALLEL_COMPUTATION:
            if (optimal_lang == "go") {
                reasons.push_back("Go's goroutines provide lightweight concurrency");
                reasons.push_back("Channels enable safe communication");
            } else if (optimal_lang == "rust") {
                reasons.push_back("Rust's fearless concurrency prevents data races");
            }
            break;

        default:
            reasons.push_back("Better language ecosystem for this task");
            break;
    }

    // Performance reasons
    if (semantic.computational_profile.is_cpu_intensive) {
        reasons.push_back("Compiled language provides significant performance advantage");
    }

    return reasons;
}

std::vector<std::string> LanguageScorer::analyzeTradeoffs(
    const std::string& from_lang,
    const std::string& to_lang
) const {
    std::vector<std::string> tradeoffs;

    // Compiled vs interpreted
    bool from_compiled = (from_lang == "rust" || from_lang == "zig" ||
                          from_lang == "cpp" || from_lang == "nim" || from_lang == "go");
    bool to_compiled = (to_lang == "rust" || to_lang == "zig" ||
                        to_lang == "cpp" || to_lang == "nim" || to_lang == "go");

    if (!from_compiled && to_compiled) {
        tradeoffs.push_back("Gain: 5-100x performance improvement");
        tradeoffs.push_back("Lose: Rapid prototyping and REPL workflow");
    } else if (from_compiled && !to_compiled) {
        tradeoffs.push_back("Gain: Faster development and easier debugging");
        tradeoffs.push_back("Lose: Runtime performance");
    }

    // Specific language tradeoffs
    if (from_lang == "python") {
        if (to_lang == "julia") {
            tradeoffs.push_back("Gain: 10-100x speedup for numerical code");
            tradeoffs.push_back("Lose: Massive Python ecosystem");
        } else if (to_lang == "rust" || to_lang == "zig") {
            tradeoffs.push_back("Gain: Memory safety and systems-level control");
            tradeoffs.push_back("Lose: Simple syntax and rapid development");
        }
    }

    if (from_lang == "javascript") {
        if (to_lang == "go") {
            tradeoffs.push_back("Gain: Better concurrency and type safety");
            tradeoffs.push_back("Lose: NPM ecosystem");
        }
    }

    if (to_lang == "shell" || to_lang == "bash") {
        tradeoffs.push_back("Gain: Concise file operations and text processing");
        tradeoffs.push_back("Lose: Complex logic and error handling");
    }

    return tradeoffs;
}

std::vector<std::string> LanguageScorer::getLanguageStrengths(
    const std::string& language,
    const std::string& task
) const {
    std::vector<std::string> strengths;

    if (language == "julia") {
        strengths.push_back("Fast numerical computing (LLVM JIT)");
        strengths.push_back("Multiple dispatch");
        strengths.push_back("Built-in linear algebra");
    } else if (language == "python") {
        strengths.push_back("Huge ecosystem (PyPI)");
        strengths.push_back("Readable syntax");
        strengths.push_back("Rich standard library");
    } else if (language == "javascript") {
        strengths.push_back("Native JSON support");
        strengths.push_back("Async/await built-in");
        strengths.push_back("NPM ecosystem");
    } else if (language == "go") {
        strengths.push_back("Goroutines (lightweight threads)");
        strengths.push_back("Fast compilation");
        strengths.push_back("Simple deployment (single binary)");
    } else if (language == "rust") {
        strengths.push_back("Memory safety without GC");
        strengths.push_back("Zero-cost abstractions");
        strengths.push_back("Fearless concurrency");
    } else if (language == "zig") {
        strengths.push_back("Explicit memory control");
        strengths.push_back("Comptime metaprogramming");
        strengths.push_back("C interop");
    } else if (language == "nim") {
        strengths.push_back("Python-like syntax with C performance");
        strengths.push_back("Multiple backends (C/C++/JS)");
        strengths.push_back("Metaprogramming");
    } else if (language == "shell" || language == "bash") {
        strengths.push_back("Purpose-built for file operations");
        strengths.push_back("Pipeline composition");
        strengths.push_back("Unix tool integration");
    } else if (language == "ruby") {
        strengths.push_back("Excellent string handling");
        strengths.push_back("Expressive syntax");
        strengths.push_back("Blocks and iterators");
    }

    return strengths;
}

std::vector<std::string> LanguageScorer::getLanguageWeaknesses(
    const std::string& language,
    const std::string& task
) const {
    std::vector<std::string> weaknesses;

    if (language == "julia") {
        weaknesses.push_back("JIT warmup time");
        weaknesses.push_back("Smaller ecosystem than Python");
    } else if (language == "python") {
        weaknesses.push_back("Slow execution (GIL for threading)");
        weaknesses.push_back("No static typing (by default)");
    } else if (language == "javascript") {
        weaknesses.push_back("Type coercion surprises");
        weaknesses.push_back("Limited numerical precision");
    } else if (language == "go") {
        weaknesses.push_back("No generics (until Go 1.18)");
        weaknesses.push_back("Verbose error handling");
    } else if (language == "rust") {
        weaknesses.push_back("Steep learning curve");
        weaknesses.push_back("Slower compilation");
    } else if (language == "zig") {
        weaknesses.push_back("Immature ecosystem");
        weaknesses.push_back("Still pre-1.0");
    } else if (language == "nim") {
        weaknesses.push_back("Smaller community");
        weaknesses.push_back("Fewer libraries");
    } else if (language == "shell" || language == "bash") {
        weaknesses.push_back("Poor error handling");
        weaknesses.push_back("Hard to maintain complex scripts");
    } else if (language == "ruby") {
        weaknesses.push_back("Slower execution");
        weaknesses.push_back("GIL limits threading");
    }

    return weaknesses;
}

std::string LanguageScorer::findOptimalLanguage(
    const std::string& task,
    int& out_score
) const {
    auto rankings = rankLanguagesForTask(task);

    if (rankings.empty()) {
        out_score = 50;
        return "python"; // Default fallback
    }

    out_score = rankings[0].score;
    return rankings[0].language;
}

} // namespace analyzer
} // namespace naab
