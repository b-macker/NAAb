#pragma once

#include <string>
#include <vector>
#include <map>

namespace naab {
namespace analyzer {

/**
 * Syntactic Analysis Result
 *
 * Contains structural metrics about code
 */
struct SyntacticProfile {
    // Loop metrics
    bool has_nested_loops = false;
    int loop_count = 0;
    int max_loop_depth = 0;
    bool has_large_iterations = false;  // range(1000000+)

    // Function metrics
    int function_count = 0;
    int max_function_depth = 0;
    bool has_recursion = false;

    // Data flow patterns
    bool has_array_operations = false;   // map/filter/reduce
    bool has_pipeline = false;           // x |> y |> z
    bool has_comprehension = false;      // [x for x in y]

    // Memory patterns
    bool allocates_memory = false;       // new/malloc
    bool manages_lifetime = false;       // delete/free
    bool uses_pointers = false;

    // Error handling
    bool has_try_catch = false;
    bool propagates_errors = false;      // ? operator, Result types
    bool has_panic = false;

    // Imports/dependencies
    std::vector<std::string> imported_modules;
    std::map<std::string, int> stdlib_usage;  // module → count
    int external_call_count = 0;

    // Complexity score (0-100)
    int complexity_score = 0;
};

/**
 * Syntactic Layer - Code Structure Analysis
 *
 * Analyzes code structure to identify complexity and patterns
 */
class SyntacticAnalyzer {
public:
    SyntacticAnalyzer();

    /**
     * Analyze code structure
     *
     * @param code Source code to analyze
     * @return Syntactic profile
     */
    SyntacticProfile analyze(const std::string& code) const;

    /**
     * Calculate complexity score from profile
     *
     * @param profile Syntactic profile
     * @return Complexity score (0-100)
     */
    int calculateComplexity(const SyntacticProfile& profile) const;

private:
    /**
     * Detect loops in code
     */
    void detectLoops(const std::string& code, SyntacticProfile& profile) const;

    /**
     * Detect functions in code
     */
    void detectFunctions(const std::string& code, SyntacticProfile& profile) const;

    /**
     * Detect data flow patterns
     */
    void detectDataFlow(const std::string& code, SyntacticProfile& profile) const;

    /**
     * Detect memory patterns
     */
    void detectMemoryPatterns(const std::string& code, SyntacticProfile& profile) const;

    /**
     * Detect error handling
     */
    void detectErrorHandling(const std::string& code, SyntacticProfile& profile) const;

    /**
     * Detect imports and modules
     */
    void detectImports(const std::string& code, SyntacticProfile& profile) const;

    /**
     * Calculate nesting depth
     */
    int calculateNestingDepth(const std::string& code) const;
};

} // namespace analyzer
} // namespace naab
