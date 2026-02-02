//
// NAAb Interpreter Optimizations Implementation - Phase 3.3.3
//

#include "naab/interpreter_optimizations.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <variant>

namespace naab {
namespace interpreter {

// ============================================================================
// Binary Operation Type Detection
// ============================================================================

BinOpType BinOpCache::detectType(const Value& left, const Value& right) {
    // Check for int + int (most common in loops)
    if (std::holds_alternative<int>(left.data) &&
        std::holds_alternative<int>(right.data)) {
        return BinOpType::INT_INT;
    }

    // Check for double + double
    if (std::holds_alternative<double>(left.data) &&
        std::holds_alternative<double>(right.data)) {
        return BinOpType::DOUBLE_DOUBLE;
    }

    // Check for int + double
    if (std::holds_alternative<int>(left.data) &&
        std::holds_alternative<double>(right.data)) {
        return BinOpType::INT_DOUBLE;
    }

    // Check for double + int
    if (std::holds_alternative<double>(left.data) &&
        std::holds_alternative<int>(right.data)) {
        return BinOpType::DOUBLE_INT;
    }

    // Check for string + string
    if (std::holds_alternative<std::string>(left.data) &&
        std::holds_alternative<std::string>(right.data)) {
        return BinOpType::STRING_STRING;
    }

    return BinOpType::MIXED;
}

// ============================================================================
// Optimization Statistics
// ============================================================================

void OptimizationStats::print() const {
    fmt::print("\n[OPTIMIZATION STATS]\n");

    // Variable lookups
    fmt::print("Variable Lookups:\n");
    fmt::print("  Total:  {}\n", total_var_lookups);
    fmt::print("  Cached: {} ({:.1f}%)\n",
               cached_var_lookups,
               getVarLookupHitRate() * 100.0);

    // Binary operations
    fmt::print("\nBinary Operations:\n");
    fmt::print("  Total:  {}\n", total_bin_ops);
    fmt::print("  Cached: {} ({:.1f}%)\n",
               cached_bin_ops,
               getBinOpHitRate() * 100.0);

    // Function calls
    fmt::print("\nFunction Calls:\n");
    fmt::print("  Total:  {}\n", total_function_calls);
    fmt::print("  Cached: {} ({:.1f}%)\n",
               cached_function_calls,
               getFunctionCallHitRate() * 100.0);

    // Overall
    size_t total_operations = total_var_lookups + total_bin_ops + total_function_calls;
    size_t total_cached = cached_var_lookups + cached_bin_ops + cached_function_calls;

    if (total_operations > 0) {
        fmt::print("\nOverall Cache Hit Rate: {:.1f}%\n",
                   (double)total_cached / total_operations * 100.0);
    }
}

} // namespace interpreter
} // namespace naab
