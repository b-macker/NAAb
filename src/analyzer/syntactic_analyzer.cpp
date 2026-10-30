#include "naab/analyzer/syntactic_analyzer.h"
#include "naab/analyzer/bounded_input.h"
#include <regex>
#include "naab/rx.h"
#include <algorithm>
#include <sstream>

namespace naab {
namespace analyzer {

SyntacticAnalyzer::SyntacticAnalyzer() {}

SyntacticProfile SyntacticAnalyzer::analyze(const std::string& raw_code,
                                             const std::string& function_name) const {
    const std::string code = boundLineLengths(raw_code);  // see bounded_input.h
    SyntacticProfile profile;

    detectLoops(code, profile);
    detectFunctions(code, profile, function_name);
    detectDataFlow(code, profile);
    detectMemoryPatterns(code, profile);
    detectErrorHandling(code, profile);
    detectImports(code, profile);

    profile.complexity_score = calculateComplexity(profile);

    return profile;
}

void SyntacticAnalyzer::detectLoops(const std::string& code, SyntacticProfile& profile) const {
    // Detect various loop constructs
    std::vector<naab::rx::regex> loop_patterns = {
        naab::rx::regex("\\bfor\\s+", naab::rx::regex::icase),
        naab::rx::regex("\\bwhile\\s+", naab::rx::regex::icase),
        naab::rx::regex("\\bloop\\s+", naab::rx::regex::icase),
        naab::rx::regex("\\.each\\s*\\{", naab::rx::regex::icase),
        naab::rx::regex("\\.map\\s*\\(", naab::rx::regex::icase),
        naab::rx::regex("\\.forEach\\s*\\(", naab::rx::regex::icase)
    };

    int total_loops = 0;
    for (const auto& pattern : loop_patterns) {
        auto begin = naab::rx::sregex_iterator(code.begin(), code.end(), pattern);
        auto end = naab::rx::sregex_iterator();
        total_loops += std::distance(begin, end);
    }

    profile.loop_count = total_loops;

    // Discount small-range literal loops (likely complexity padding)
    // "for x in 0..N" where N <= 3 gets reduced score
    static const naab::rx::regex small_range_loop(R"(\bfor\s+\w+\s+in\s+0\.\.([0-9]+)\b)");
    int padding_loops = 0;
    {
        auto sr_begin = naab::rx::sregex_iterator(code.begin(), code.end(), small_range_loop);
        auto sr_end = naab::rx::sregex_iterator();
        for (auto it = sr_begin; it != sr_end; ++it) {
            int range_end = std::stoi((*it)[1].str());
            if (range_end <= 3) padding_loops++;
        }
    }
    profile.padding_loop_count = padding_loops;

    // Check for nested loops (rough heuristic: count indentation levels with loops)
    naab::rx::regex nested_for("\\bfor\\s+.*\\n.*\\bfor\\s+");
    profile.has_nested_loops = naab::rx::regex_search(code, nested_for);

    // Check for large iterations
    naab::rx::regex large_iter("range\\s*\\(\\s*[0-9]{6,}");  // 1000000+
    profile.has_large_iterations = naab::rx::regex_search(code, large_iter);

    profile.max_loop_depth = calculateNestingDepth(code);
}

void SyntacticAnalyzer::detectFunctions(const std::string& code, SyntacticProfile& profile,
                                         const std::string& function_name) const {
    // Detect function definitions
    std::vector<naab::rx::regex> function_patterns = {
        naab::rx::regex("\\bdef\\s+\\w+", naab::rx::regex::icase),         // Python
        naab::rx::regex("\\bfunction\\s+\\w+", naab::rx::regex::icase),    // JS, Julia
        naab::rx::regex("\\bfn\\s+\\w+", naab::rx::regex::icase),          // Rust, Nim
        naab::rx::regex("\\bproc\\s+\\w+", naab::rx::regex::icase),        // Nim, Tcl
        naab::rx::regex("\\bfunc\\s+\\w+", naab::rx::regex::icase)         // Go
    };

    int total_functions = 0;
    for (const auto& pattern : function_patterns) {
        auto begin = naab::rx::sregex_iterator(code.begin(), code.end(), pattern);
        auto end = naab::rx::sregex_iterator();
        total_functions += std::distance(begin, end);
    }

    profile.function_count = total_functions;

    // Check for recursion (function calling itself)
    if (!function_name.empty()) {
        // Direct detection: does the body contain a self-call?
        naab::rx::regex self_call("\\b" + function_name + "\\s*\\(");
        profile.has_recursion = naab::rx::regex_search(code, self_call);
    } else {
        // No function name provided — can't reliably detect recursion
        profile.has_recursion = false;
    }

    profile.max_function_depth = std::min(profile.max_loop_depth, 5);
}

void SyntacticAnalyzer::detectDataFlow(const std::string& code, SyntacticProfile& profile) const {
    // Array operations
    naab::rx::regex array_ops("\\.(map|filter|reduce|fold|scan|collect)\\s*\\(");
    profile.has_array_operations = naab::rx::regex_search(code, array_ops);

    // Pipeline operator — only match NAAb's actual |> operator
    // (NOT ||, >>, ->, or bare |, which are different operators)
    naab::rx::regex pipeline_op("\\|>");
    auto pipe_begin = naab::rx::sregex_iterator(code.begin(), code.end(), pipeline_op);
    auto pipe_end = naab::rx::sregex_iterator();
    profile.pipeline_count = static_cast<int>(std::distance(pipe_begin, pipe_end));
    profile.has_pipeline = profile.pipeline_count > 0;

    // List/dict comprehensions
    naab::rx::regex comprehension("\\[.*\\bfor\\b.*\\bin\\b.*\\]");
    profile.has_comprehension = naab::rx::regex_search(code, comprehension);
}

void SyntacticAnalyzer::detectMemoryPatterns(const std::string& code, SyntacticProfile& profile) const {
    // Memory allocation
    naab::rx::regex alloc_patterns("\\b(malloc|calloc|realloc|alloc|new)\\s*\\(");
    profile.allocates_memory = naab::rx::regex_search(code, alloc_patterns);

    // Memory deallocation
    naab::rx::regex dealloc_patterns("\\b(free|delete|drop)\\s*\\(");
    profile.manages_lifetime = naab::rx::regex_search(code, dealloc_patterns);

    // Pointer usage
    naab::rx::regex pointer_patterns("\\*\\w+|\\&\\w+|\\bpointer\\b|\\bptr\\b");
    profile.uses_pointers = naab::rx::regex_search(code, pointer_patterns);
}

void SyntacticAnalyzer::detectErrorHandling(const std::string& code, SyntacticProfile& profile) const {
    // Try-catch blocks
    naab::rx::regex try_catch("\\btry\\s*\\{|\\bcatch\\s*\\(");
    profile.has_try_catch = naab::rx::regex_search(code, try_catch);

    // Error propagation
    naab::rx::regex error_prop("\\?(?!\\s*:)|Result<|Option<|Either<");
    profile.propagates_errors = naab::rx::regex_search(code, error_prop);

    // Panic/abort
    naab::rx::regex panic_patterns("\\b(panic|abort|die|fatal|exit)\\s*\\(");
    profile.has_panic = naab::rx::regex_search(code, panic_patterns);
}

void SyntacticAnalyzer::detectImports(const std::string& code, SyntacticProfile& profile) const {
    // Import statements (various languages)
    std::vector<naab::rx::regex> import_patterns = {
        naab::rx::regex("\\bimport\\s+(\\w+)", naab::rx::regex::icase),
        naab::rx::regex("\\bfrom\\s+(\\w+)\\s+import", naab::rx::regex::icase),
        naab::rx::regex("\\busing\\s+(\\w+)", naab::rx::regex::icase),
        naab::rx::regex("\\brequire\\s*\\(['\"]([\\w/]+)['\"]\\)", naab::rx::regex::icase),
        naab::rx::regex("#include\\s*[<\"]([\\w/.]+)[>\"]", naab::rx::regex::icase)
    };

    for (const auto& pattern : import_patterns) {
        auto begin = naab::rx::sregex_iterator(code.begin(), code.end(), pattern);
        auto end = naab::rx::sregex_iterator();

        for (auto it = begin; it != end; ++it) {
            naab::rx::smatch match = *it;
            if (match.size() > 1) {
                std::string module = match[1].str();
                profile.imported_modules.push_back(module);
                profile.stdlib_usage[module]++;
            }
        }
    }

    // Count external function calls (dot-notation: math.max(), array.filter_fn())
    naab::rx::regex dotcall_pat("\\w+\\.\\w+\\s*\\(");
    auto dot_begin = naab::rx::sregex_iterator(code.begin(), code.end(), dotcall_pat);
    auto dot_end = naab::rx::sregex_iterator();
    int dot_calls = std::distance(dot_begin, dot_end);

    // Count plain function calls (user-defined: check_resources(), create_task())
    // Exclude: language keywords, builtins, control flow, type casts
    static const naab::rx::regex plaincall_pat(
        "\\b(?!if|else|for|while|do|return|fn|function|func|def|proc|"
        "let|var|const|import|use|export|struct|enum|main|"
        "try|catch|throw|match|new|break|continue|"
        "typeof|type|len|range|int|float|string|bool|print|println|echo|fmt|"
        "class|switch|case|default|null|true|false|"
        "and|or|not|in|is|as|from|with|yield|async|await|"
        "package|require|include|using)"
        "([a-zA-Z_]\\w*)\\s*\\(",
        naab::rx::regex::icase);
    auto plain_begin = naab::rx::sregex_iterator(code.begin(), code.end(), plaincall_pat);
    auto plain_end = naab::rx::sregex_iterator();
    int plain_calls = std::distance(plain_begin, plain_end);

    profile.external_call_count = dot_calls + plain_calls;
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

    // Loop complexity (discount small-range padding loops)
    int real_loops = profile.loop_count - profile.padding_loop_count;
    score += real_loops * 5;
    score += profile.padding_loop_count * 1;  // padding loops get minimal credit
    if (profile.has_nested_loops) score += 15;
    if (profile.has_large_iterations) score += 20;

    // Function complexity
    score += profile.function_count * 3;
    if (profile.has_recursion) score += 10;

    // Data flow complexity
    if (profile.has_array_operations) score += 5;
    score += std::min(profile.pipeline_count * 3, 15);  // +3 per |> stage, cap at 15
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
