#include "naab/analyzer/syntactic_analyzer.h"
#include <regex>
#include <algorithm>
#include <sstream>

namespace naab {
namespace analyzer {

SyntacticAnalyzer::SyntacticAnalyzer() {}

SyntacticProfile SyntacticAnalyzer::analyze(const std::string& code) const {
    SyntacticProfile profile;

    detectLoops(code, profile);
    detectFunctions(code, profile);
    detectDataFlow(code, profile);
    detectMemoryPatterns(code, profile);
    detectErrorHandling(code, profile);
    detectImports(code, profile);

    profile.complexity_score = calculateComplexity(profile);

    return profile;
}

void SyntacticAnalyzer::detectLoops(const std::string& code, SyntacticProfile& profile) const {
    // Detect various loop constructs
    std::vector<std::regex> loop_patterns = {
        std::regex("\\bfor\\s+", std::regex::icase),
        std::regex("\\bwhile\\s+", std::regex::icase),
        std::regex("\\bloop\\s+", std::regex::icase),
        std::regex("\\.each\\s*\\{", std::regex::icase),
        std::regex("\\.map\\s*\\(", std::regex::icase),
        std::regex("\\.forEach\\s*\\(", std::regex::icase)
    };

    int total_loops = 0;
    for (const auto& pattern : loop_patterns) {
        auto begin = std::sregex_iterator(code.begin(), code.end(), pattern);
        auto end = std::sregex_iterator();
        total_loops += std::distance(begin, end);
    }

    profile.loop_count = total_loops;

    // Check for nested loops (rough heuristic: count indentation levels with loops)
    std::regex nested_for("\\bfor\\s+.*\\n.*\\bfor\\s+");
    profile.has_nested_loops = std::regex_search(code, nested_for);

    // Check for large iterations
    std::regex large_iter("range\\s*\\(\\s*[0-9]{6,}");  // 1000000+
    profile.has_large_iterations = std::regex_search(code, large_iter);

    profile.max_loop_depth = calculateNestingDepth(code);
}

void SyntacticAnalyzer::detectFunctions(const std::string& code, SyntacticProfile& profile) const {
    // Detect function definitions
    std::vector<std::regex> function_patterns = {
        std::regex("\\bdef\\s+\\w+", std::regex::icase),         // Python
        std::regex("\\bfunction\\s+\\w+", std::regex::icase),    // JS, Julia
        std::regex("\\bfn\\s+\\w+", std::regex::icase),          // Rust, Nim
        std::regex("\\bproc\\s+\\w+", std::regex::icase),        // Nim, Tcl
        std::regex("\\bfunc\\s+\\w+", std::regex::icase)         // Go
    };

    int total_functions = 0;
    for (const auto& pattern : function_patterns) {
        auto begin = std::sregex_iterator(code.begin(), code.end(), pattern);
        auto end = std::sregex_iterator();
        total_functions += std::distance(begin, end);
    }

    profile.function_count = total_functions;

    // Check for recursion (function calling itself)
    // Simple heuristic: if function names appear multiple times
    std::regex func_call("\\w+\\s*\\(");
    auto begin = std::sregex_iterator(code.begin(), code.end(), func_call);
    auto end = std::sregex_iterator();
    int call_count = std::distance(begin, end);
    profile.has_recursion = (call_count > total_functions * 2); // Rough heuristic

    profile.max_function_depth = std::min(profile.max_loop_depth, 5);
}

void SyntacticAnalyzer::detectDataFlow(const std::string& code, SyntacticProfile& profile) const {
    // Array operations
    std::regex array_ops("\\.(map|filter|reduce|fold|scan|collect)\\s*\\(");
    profile.has_array_operations = std::regex_search(code, array_ops);

    // Pipeline operator
    std::regex pipeline("\\|>|->|>>|\\|");
    profile.has_pipeline = std::regex_search(code, pipeline);

    // List/dict comprehensions
    std::regex comprehension("\\[.*\\bfor\\b.*\\bin\\b.*\\]");
    profile.has_comprehension = std::regex_search(code, comprehension);
}

void SyntacticAnalyzer::detectMemoryPatterns(const std::string& code, SyntacticProfile& profile) const {
    // Memory allocation
    std::regex alloc_patterns("\\b(malloc|calloc|realloc|alloc|new)\\s*\\(");
    profile.allocates_memory = std::regex_search(code, alloc_patterns);

    // Memory deallocation
    std::regex dealloc_patterns("\\b(free|delete|drop)\\s*\\(");
    profile.manages_lifetime = std::regex_search(code, dealloc_patterns);

    // Pointer usage
    std::regex pointer_patterns("\\*\\w+|\\&\\w+|\\bpointer\\b|\\bptr\\b");
    profile.uses_pointers = std::regex_search(code, pointer_patterns);
}

void SyntacticAnalyzer::detectErrorHandling(const std::string& code, SyntacticProfile& profile) const {
    // Try-catch blocks
    std::regex try_catch("\\btry\\s*\\{|\\bcatch\\s*\\(");
    profile.has_try_catch = std::regex_search(code, try_catch);

    // Error propagation
    std::regex error_prop("\\?(?!\\s*:)|Result<|Option<|Either<");
    profile.propagates_errors = std::regex_search(code, error_prop);

    // Panic/abort
    std::regex panic_patterns("\\b(panic|abort|die|fatal|exit)\\s*\\(");
    profile.has_panic = std::regex_search(code, panic_patterns);
}

void SyntacticAnalyzer::detectImports(const std::string& code, SyntacticProfile& profile) const {
    // Import statements (various languages)
    std::vector<std::regex> import_patterns = {
        std::regex("\\bimport\\s+(\\w+)", std::regex::icase),
        std::regex("\\bfrom\\s+(\\w+)\\s+import", std::regex::icase),
        std::regex("\\busing\\s+(\\w+)", std::regex::icase),
        std::regex("\\brequire\\s*\\(['\"]([\\w/]+)['\"]\\)", std::regex::icase),
        std::regex("#include\\s*[<\"]([\\w/.]+)[>\"]", std::regex::icase)
    };

    for (const auto& pattern : import_patterns) {
        auto begin = std::sregex_iterator(code.begin(), code.end(), pattern);
        auto end = std::sregex_iterator();

        for (auto it = begin; it != end; ++it) {
            std::smatch match = *it;
            if (match.size() > 1) {
                std::string module = match[1].str();
                profile.imported_modules.push_back(module);
                profile.stdlib_usage[module]++;
            }
        }
    }

    // Count external function calls
    std::regex external_call("\\w+\\.\\w+\\s*\\(");
    auto begin = std::sregex_iterator(code.begin(), code.end(), external_call);
    auto end = std::sregex_iterator();
    profile.external_call_count = std::distance(begin, end);
}

int SyntacticAnalyzer::calculateNestingDepth(const std::string& code) const {
    int max_depth = 0;
    int current_depth = 0;

    for (char c : code) {
        if (c == '{' || c == '(') {
            current_depth++;
            max_depth = std::max(max_depth, current_depth);
        } else if (c == '}' || c == ')') {
            current_depth = std::max(0, current_depth - 1);
        }
    }

    return max_depth;
}

int SyntacticAnalyzer::calculateComplexity(const SyntacticProfile& profile) const {
    int score = 0;

    // Loop complexity
    score += profile.loop_count * 5;
    if (profile.has_nested_loops) score += 15;
    if (profile.has_large_iterations) score += 20;

    // Function complexity
    score += profile.function_count * 3;
    if (profile.has_recursion) score += 10;

    // Data flow complexity
    if (profile.has_array_operations) score += 5;
    if (profile.has_pipeline) score += 5;
    if (profile.has_comprehension) score += 5;

    // Memory management complexity
    if (profile.allocates_memory) score += 10;
    if (profile.manages_lifetime) score += 10;
    if (profile.uses_pointers) score += 15;

    // Error handling complexity
    if (profile.has_try_catch) score += 5;
    if (profile.propagates_errors) score += 5;

    // Module complexity
    score += profile.imported_modules.size() * 2;
    score += profile.external_call_count;

    // Cap at 100
    return std::min(score, 100);
}

} // namespace analyzer
} // namespace naab
