// Performance Optimizer Header
// Caching, parallelization, and hot path optimizations for pattern detection

#pragma once

#include <regex>
#include <vector>
#include <string>
#include <unordered_map>
#include <optional>
#include <chrono>
#include <shared_mutex>
#include <atomic>

namespace naab {
namespace analyzer {

// ============================================================================
// Regex Cache - Thread-safe compiled regex caching
// ============================================================================

class RegexCache {
public:
    static RegexCache& instance();

    // Get or compile regex pattern (thread-safe)
    std::regex& getRegex(const std::string& pattern);

    // Statistics
    size_t getHitCount() const;
    size_t getMissCount() const;
    double getHitRate() const;
    void reset();

private:
    RegexCache() = default;
    RegexCache(const RegexCache&) = delete;
    RegexCache& operator=(const RegexCache&) = delete;

    std::unordered_map<std::string, std::regex> cache_;
    mutable std::shared_mutex mutex_;
    std::atomic<size_t> hit_count_{0};
    std::atomic<size_t> miss_count_{0};
};

// ============================================================================
// Token Pattern Cache - Caches pattern matching results
// ============================================================================

class TokenPatternCache {
public:
    struct CacheEntry {
        std::vector<std::string> matched_patterns;
        int score;
        std::chrono::steady_clock::time_point timestamp;
    };

    static TokenPatternCache& instance();

    // Get cached result (returns nullopt if not found or expired)
    std::optional<CacheEntry> get(const std::string& code_hash);

    // Cache a result
    void put(const std::string& code_hash, const CacheEntry& entry);

    // Statistics
    size_t getHitCount() const;
    size_t getMissCount() const;

private:
    TokenPatternCache() = default;
    void evictOldest();

    std::unordered_map<std::string, CacheEntry> cache_;
    mutable std::shared_mutex mutex_;
    std::atomic<size_t> hit_count_{0};
    std::atomic<size_t> miss_count_{0};
    static constexpr size_t max_cache_size_ = 1000;
};

// ============================================================================
// Parallel Pattern Matcher - Multi-threaded pattern matching
// ============================================================================

class ParallelPatternMatcher {
public:
    // Match multiple patterns in parallel (automatically chooses serial/parallel)
    static std::vector<std::string> matchPatterns(
        const std::string& code,
        const std::vector<std::string>& patterns
    );

private:
    static std::vector<std::string> matchPatternsSerial(
        const std::string& code,
        const std::vector<std::string>& patterns
    );
};

// ============================================================================
// Fast Hash - Quick hashing for code deduplication
// ============================================================================

class FastHash {
public:
    // FNV-1a hash - fast and good distribution
    static std::string hash(const std::string& str);
};

// ============================================================================
// Fast Scorer - Hot path optimization for common scoring combinations
// ============================================================================

class FastScorer {
public:
    // Optimized scoring for common task→language pairs
    static int scoreTaskLanguageFast(
        const std::string& task,
        const std::string& language
    );

private:
    static int scoreTaskLanguageSlow(
        const std::string& task,
        const std::string& language
    );
};

// ============================================================================
// Performance Profiler - Track execution time of detection phases
// ============================================================================

class PerformanceProfiler {
public:
    struct ProfileEntry {
        std::string name;
        std::chrono::microseconds duration;
        size_t call_count;

        double getAverageMicroseconds() const {
            return call_count > 0 ?
                static_cast<double>(duration.count()) / static_cast<double>(call_count) : 0.0;
        }
    };

    static PerformanceProfiler& instance();

    void recordDuration(const std::string& name, std::chrono::microseconds duration);
    std::vector<ProfileEntry> getProfiles() const;
    void reset();

private:
    PerformanceProfiler() = default;
    std::unordered_map<std::string, ProfileEntry> profiles_;
    mutable std::mutex mutex_;
};

// RAII profiler helper
class ScopedProfiler {
public:
    explicit ScopedProfiler(const std::string& name);
    ~ScopedProfiler();

private:
    std::string name_;
    std::chrono::steady_clock::time_point start_;
};

// Macro for easy profiling
#define PROFILE_SCOPE(name) ScopedProfiler profiler_##__LINE__(name)

// ============================================================================
// Object Pool - Memory pool for detection results
// ============================================================================

template<typename T>
class ObjectPool {
public:
    static ObjectPool& instance();

    T* acquire();
    void release(T* obj);

    size_t getAllocations() const;
    size_t getReuses() const;

private:
    ObjectPool() = default;
    ~ObjectPool();

    std::vector<T*> pool_;
    std::mutex mutex_;
    std::atomic<size_t> allocations_{0};
    std::atomic<size_t> reuses_{0};
    static constexpr size_t max_pool_size_ = 100;
};

// ============================================================================
// Optimization Statistics - Print all optimization stats
// ============================================================================

class OptimizationStats {
public:
    static OptimizationStats& instance();
    void print() const;

private:
    OptimizationStats() = default;
};

// ============================================================================
// Optimized Detection Integration
// ============================================================================

// Wrapper class that integrates all optimizations
class OptimizedPatternDetector {
public:
    struct DetectionResult {
        std::vector<std::string> matched_patterns;
        int task_score;
        int language_score;
        std::chrono::microseconds detection_time;
    };

    // Detect patterns with all optimizations enabled
    static DetectionResult detectPatterns(
        const std::string& code,
        const std::string& language,
        const std::vector<std::string>& patterns
    );

    // Get optimization statistics
    static void printStatistics();
};

} // namespace analyzer
} // namespace naab
