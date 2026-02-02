//
// NAAb Interpreter Optimizations - Phase 3.3.3
// Performance optimizations for hot paths
//

#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <cstddef>

namespace naab {
namespace interpreter {

// Forward declarations
class Value;
class Environment;

// ============================================================================
// Inline Cache for Variable Lookups
// ============================================================================

// Cache entry for variable lookup
struct VarLookupCache {
    std::string var_name;
    std::shared_ptr<Environment> cached_env;  // Environment where variable was found
    std::shared_ptr<Value> cached_value;
    size_t hit_count = 0;
    size_t miss_count = 0;

    VarLookupCache() = default;
    VarLookupCache(std::string name) : var_name(std::move(name)) {}

    // Try to use cached lookup
    inline std::shared_ptr<Value> tryGet(std::shared_ptr<Environment> current_env) {
        // Fast path: check if still in cached environment
        if (cached_env && cached_env == current_env) {
            hit_count++;
            return cached_value;
        }

        miss_count++;
        return nullptr;  // Cache miss
    }

    // Update cache after successful lookup
    inline void update(std::shared_ptr<Environment> env, std::shared_ptr<Value> value) {
        cached_env = env;
        cached_value = value;
    }

    // Get hit rate
    inline double getHitRate() const {
        size_t total = hit_count + miss_count;
        return total > 0 ? (double)hit_count / total : 0.0;
    }
};

// ============================================================================
// Inline Cache for Binary Operations
// ============================================================================

// Type pair for binary operation caching
enum class BinOpType {
    INT_INT,
    DOUBLE_DOUBLE,
    INT_DOUBLE,
    DOUBLE_INT,
    STRING_STRING,
    MIXED
};

struct BinOpCache {
    BinOpType last_type = BinOpType::MIXED;
    size_t hit_count = 0;
    size_t miss_count = 0;

    // Detect type pair
    inline BinOpType detectType(const Value& left, const Value& right);

    // Check if types match cached type
    inline bool matches(BinOpType type) {
        if (type == last_type) {
            hit_count++;
            return true;
        }
        miss_count++;
        last_type = type;
        return false;
    }

    inline double getHitRate() const {
        size_t total = hit_count + miss_count;
        return total > 0 ? (double)hit_count / total : 0.0;
    }
};

// ============================================================================
// Inline Cache for Function Calls
// ============================================================================

struct FunctionCallCache {
    std::string function_name;
    void* cached_function_ptr = nullptr;  // Raw pointer for fast comparison
    size_t hit_count = 0;
    size_t miss_count = 0;

    FunctionCallCache() = default;
    explicit FunctionCallCache(std::string name) : function_name(std::move(name)) {}

    // Try to use cached function
    inline bool tryHit(void* function_ptr) {
        if (cached_function_ptr == function_ptr) {
            hit_count++;
            return true;
        }
        miss_count++;
        cached_function_ptr = function_ptr;
        return false;
    }

    inline double getHitRate() const {
        size_t total = hit_count + miss_count;
        return total > 0 ? (double)hit_count / total : 0.0;
    }
};

// ============================================================================
// Optimization Statistics
// ============================================================================

struct OptimizationStats {
    size_t total_var_lookups = 0;
    size_t cached_var_lookups = 0;
    size_t total_bin_ops = 0;
    size_t cached_bin_ops = 0;
    size_t total_function_calls = 0;
    size_t cached_function_calls = 0;

    void reset() {
        total_var_lookups = 0;
        cached_var_lookups = 0;
        total_bin_ops = 0;
        cached_bin_ops = 0;
        total_function_calls = 0;
        cached_function_calls = 0;
    }

    double getVarLookupHitRate() const {
        return total_var_lookups > 0 ? (double)cached_var_lookups / total_var_lookups : 0.0;
    }

    double getBinOpHitRate() const {
        return total_bin_ops > 0 ? (double)cached_bin_ops / total_bin_ops : 0.0;
    }

    double getFunctionCallHitRate() const {
        return total_function_calls > 0 ? (double)cached_function_calls / total_function_calls : 0.0;
    }

    void print() const;
};

// ============================================================================
// Hot Path Hints (compiler optimization hints)
// ============================================================================

// Branch prediction hints (already used in code, but centralized here)
#if defined(__GNUC__) || defined(__clang__)
    #define LIKELY(x)   __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define LIKELY(x)   (x)
    #define UNLIKELY(x) (x)
#endif

// Force inline for hot paths
#if defined(__GNUC__) || defined(__clang__)
    #define FORCE_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
    #define FORCE_INLINE __forceinline
#else
    #define FORCE_INLINE inline
#endif

// Prefetch hint for data that will be needed soon
#if defined(__GNUC__) || defined(__clang__)
    #define PREFETCH(addr) __builtin_prefetch(addr)
#else
    #define PREFETCH(addr) (void)(addr)
#endif

} // namespace interpreter
} // namespace naab
