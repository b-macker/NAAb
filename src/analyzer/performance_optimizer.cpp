// Performance Optimizer for Pattern Detection System
// Implements caching, parallelization, and hot path optimizations

#include <regex>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <future>
#include <chrono>
#include <iostream>
#include <iomanip>

namespace naab {
namespace analyzer {

// ============================================================================
// OPTIMIZATION 1: Regex Cache
// ============================================================================

class RegexCache {
public:
    static RegexCache& instance() {
        static RegexCache cache;
        return cache;
    }

    // Get or compile regex pattern
    std::regex& getRegex(const std::string& pattern) {
        // Fast path: Check if already cached (shared lock)
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = cache_.find(pattern);
            if (it != cache_.end()) {
                hit_count_++;
                return it->second;
            }
        }

        // Slow path: Compile and cache (exclusive lock)
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            // Double-check after acquiring exclusive lock
            auto it = cache_.find(pattern);
            if (it != cache_.end()) {
                return it->second;
            }

            miss_count_++;
            auto result = cache_.emplace(pattern, std::regex(pattern));
            return result.first->second;
        }
    }

    // Statistics
    size_t getHitCount() const { return hit_count_; }
    size_t getMissCount() const { return miss_count_; }
    double getHitRate() const {
        size_t total = hit_count_ + miss_count_;
        return total > 0 ? static_cast<double>(hit_count_) / total : 0.0;
    }

    void reset() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        cache_.clear();
        hit_count_ = 0;
        miss_count_ = 0;
    }

private:
    RegexCache() = default;
    std::unordered_map<std::string, std::regex> cache_;
    mutable std::shared_mutex mutex_;
    std::atomic<size_t> hit_count_{0};
    std::atomic<size_t> miss_count_{0};
};

// ============================================================================
// OPTIMIZATION 2: Token Pattern Cache
// ============================================================================

class TokenPatternCache {
public:
    struct CacheEntry {
        std::vector<std::string> matched_patterns;
        int score;
        std::chrono::steady_clock::time_point timestamp;
    };

    static TokenPatternCache& instance() {
        static TokenPatternCache cache;
        return cache;
    }

    // Get cached result or compute
    std::optional<CacheEntry> get(const std::string& code_hash) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = cache_.find(code_hash);
        if (it != cache_.end()) {
            // Check if entry is still fresh (5 minute TTL)
            auto now = std::chrono::steady_clock::now();
            auto age = std::chrono::duration_cast<std::chrono::minutes>(
                now - it->second.timestamp).count();

            if (age < 5) {
                hit_count_++;
                return it->second;
            } else {
                // Expired
                miss_count_++;
                return std::nullopt;
            }
        }
        miss_count_++;
        return std::nullopt;
    }

    void put(const std::string& code_hash, const CacheEntry& entry) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        cache_[code_hash] = entry;

        // Limit cache size to prevent unbounded growth
        if (cache_.size() > max_cache_size_) {
            evictOldest();
        }
    }

    size_t getHitCount() const { return hit_count_; }
    size_t getMissCount() const { return miss_count_; }

private:
    TokenPatternCache() = default;

    void evictOldest() {
        // Remove oldest 10% of entries
        size_t to_remove = max_cache_size_ / 10;
        std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> entries;

        for (const auto& [key, value] : cache_) {
            entries.push_back({key, value.timestamp});
        }

        std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

        for (size_t i = 0; i < to_remove && i < entries.size(); ++i) {
            cache_.erase(entries[i].first);
        }
    }

    std::unordered_map<std::string, CacheEntry> cache_;
    mutable std::shared_mutex mutex_;
    std::atomic<size_t> hit_count_{0};
    std::atomic<size_t> miss_count_{0};
    static constexpr size_t max_cache_size_ = 1000;
};

// ============================================================================
// OPTIMIZATION 3: Parallel Pattern Matching
// ============================================================================

class ParallelPatternMatcher {
public:
    // Match multiple patterns in parallel
    static std::vector<std::string> matchPatterns(
        const std::string& code,
        const std::vector<std::string>& patterns
    ) {
        // For small pattern sets, serial is faster (avoid threading overhead)
        if (patterns.size() < 10) {
            return matchPatternsSerial(code, patterns);
        }

        // Split patterns into chunks for parallel processing
        size_t num_threads = std::min(
            patterns.size(),
            static_cast<size_t>(std::thread::hardware_concurrency())
        );

        std::vector<std::future<std::vector<std::string>>> futures;
        size_t chunk_size = (patterns.size() + num_threads - 1) / num_threads;

        for (size_t i = 0; i < patterns.size(); i += chunk_size) {
            size_t end = std::min(i + chunk_size, patterns.size());
            std::vector<std::string> chunk(patterns.begin() + i, patterns.begin() + end);

            futures.push_back(std::async(std::launch::async,
                matchPatternsSerial, code, chunk));
        }

        // Collect results
        std::vector<std::string> results;
        for (auto& future : futures) {
            auto chunk_results = future.get();
            results.insert(results.end(), chunk_results.begin(), chunk_results.end());
        }

        return results;
    }

private:
    static std::vector<std::string> matchPatternsSerial(
        const std::string& code,
        const std::vector<std::string>& patterns
    ) {
        std::vector<std::string> matches;
        for (const auto& pattern : patterns) {
            try {
                auto& regex = RegexCache::instance().getRegex(pattern);
                if (std::regex_search(code, regex)) {
                    matches.push_back(pattern);
                }
            } catch (const std::regex_error&) {
                // Skip invalid patterns
                continue;
            }
        }
        return matches;
    }
};

// ============================================================================
// OPTIMIZATION 4: Fast Hash for Code Deduplication
// ============================================================================

class FastHash {
public:
    // FNV-1a hash - fast and good distribution
    static std::string hash(const std::string& str) {
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : str) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }

        char buf[17];
        snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)hash);
        return std::string(buf);
    }
};

// ============================================================================
// OPTIMIZATION 5: Hot Path Scoring Optimization
// ============================================================================

class FastScorer {
public:
    // Optimized scoring for common cases
    static int scoreTaskLanguageFast(
        const std::string& task,
        const std::string& language
    ) {
        // Hot path: Pre-computed scores for most common combinations
        static const std::unordered_map<std::string, int> hot_scores = {
            // Numerical computation
            {"numerical_julia", 100},
            {"numerical_nim", 95},
            {"numerical_python", 70},
            {"numerical_javascript", 30},

            // String processing
            {"string_python", 100},
            {"string_ruby", 95},
            {"string_javascript", 85},
            {"string_zig", 40},

            // File operations
            {"file_shell", 100},
            {"file_python", 80},
            {"file_javascript", 40},

            // Systems programming
            {"systems_zig", 100},
            {"systems_rust", 95},
            {"systems_python", 20},

            // Web APIs
            {"web_javascript", 100},
            {"web_python", 90},
            {"web_zig", 40},

            // Concurrency
            {"concurrency_go", 100},
            {"concurrency_rust", 95},
            {"concurrency_python", 50},
        };

        std::string key = task + "_" + language;
        auto it = hot_scores.find(key);
        if (it != hot_scores.end()) {
            return it->second;
        }

        // Cold path: Fall back to full scoring algorithm
        return scoreTaskLanguageSlow(task, language);
    }

private:
    static int scoreTaskLanguageSlow(
        const std::string& task,
        const std::string& language
    ) {
        // Placeholder for full scoring algorithm
        // This would call into the comprehensive scoring system
        (void)task;  // Suppress unused parameter warning
        (void)language;  // Suppress unused parameter warning
        return 50; // Default neutral score
    }
};

// ============================================================================
// OPTIMIZATION 6: Performance Profiler
// ============================================================================

class PerformanceProfiler {
public:
    struct ProfileEntry {
        std::string name;
        std::chrono::microseconds duration;
        size_t call_count;
    };

    static PerformanceProfiler& instance() {
        static PerformanceProfiler profiler;
        return profiler;
    }

    void recordDuration(const std::string& name, std::chrono::microseconds duration) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto& entry = profiles_[name];
        entry.name = name;
        entry.duration += duration;
        entry.call_count++;
    }

    std::vector<ProfileEntry> getProfiles() const {
        std::unique_lock<std::mutex> lock(mutex_);
        std::vector<ProfileEntry> entries;
        for (const auto& [name, entry] : profiles_) {
            entries.push_back(entry);
        }

        // Sort by total duration (descending)
        std::sort(entries.begin(), entries.end(),
            [](const ProfileEntry& a, const ProfileEntry& b) {
                return a.duration > b.duration;
            });

        return entries;
    }

    void reset() {
        std::unique_lock<std::mutex> lock(mutex_);
        profiles_.clear();
    }

private:
    PerformanceProfiler() = default;
    std::unordered_map<std::string, ProfileEntry> profiles_;
    mutable std::mutex mutex_;
};

// RAII profiler helper
class ScopedProfiler {
public:
    explicit ScopedProfiler(const std::string& name)
        : name_(name), start_(std::chrono::steady_clock::now()) {}

    ~ScopedProfiler() {
        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start_);
        PerformanceProfiler::instance().recordDuration(name_, duration);
    }

private:
    std::string name_;
    std::chrono::steady_clock::time_point start_;
};

// Macro for easy profiling
#define PROFILE_SCOPE(name) ScopedProfiler profiler_##__LINE__(name)

// ============================================================================
// OPTIMIZATION 7: Memory Pool for Detection Results
// ============================================================================

template<typename T>
class ObjectPool {
public:
    static ObjectPool& instance() {
        static ObjectPool pool;
        return pool;
    }

    T* acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (pool_.empty()) {
            allocations_++;
            return new T();
        } else {
            reuses_++;
            T* obj = pool_.back();
            pool_.pop_back();
            return obj;
        }
    }

    void release(T* obj) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (pool_.size() < max_pool_size_) {
            pool_.push_back(obj);
        } else {
            delete obj;
        }
    }

    size_t getAllocations() const { return allocations_; }
    size_t getReuses() const { return reuses_; }

private:
    ObjectPool() = default;
    ~ObjectPool() {
        for (T* obj : pool_) {
            delete obj;
        }
    }

    std::vector<T*> pool_;
    std::mutex mutex_;
    std::atomic<size_t> allocations_{0};
    std::atomic<size_t> reuses_{0};
    static constexpr size_t max_pool_size_ = 100;
};

// ============================================================================
// OPTIMIZATION 8: Statistics Tracking
// ============================================================================

class OptimizationStats {
public:
    static OptimizationStats& instance() {
        static OptimizationStats stats;
        return stats;
    }

    void print() const {
        auto& regex_cache = RegexCache::instance();
        auto& token_cache = TokenPatternCache::instance();

        std::cout << "\n╔═══════════════════════════════════════════════════════╗\n";
        std::cout << "║       Performance Optimization Statistics            ║\n";
        std::cout << "╠═══════════════════════════════════════════════════════╣\n";
        std::cout << "║  Regex Cache:                                         ║\n";
        std::cout << "║    Hit rate:    " << std::fixed << std::setprecision(1) << (regex_cache.getHitRate() * 100.0) << "%\n";
        std::cout << "║    Hits:        " << regex_cache.getHitCount() << "\n";
        std::cout << "║    Misses:      " << regex_cache.getMissCount() << "\n";
        std::cout << "║                                                       ║\n";
        std::cout << "║  Token Pattern Cache:                                 ║\n";
        std::cout << "║    Hits:        " << token_cache.getHitCount() << "\n";
        std::cout << "║    Misses:      " << token_cache.getMissCount() << "\n";
        std::cout << "╚═══════════════════════════════════════════════════════╝\n";
    }
};

} // namespace analyzer
} // namespace naab
