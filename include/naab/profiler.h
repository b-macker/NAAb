#ifndef NAAB_PROFILER_H
#define NAAB_PROFILER_H

// NAAb Performance Profiler
// Tracks execution time and statistics for functions and blocks

#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <memory>

namespace naab {
namespace profiling {

// High-resolution timer
class Timer {
public:
    Timer();

    void start();
    void stop();
    void reset();

    double elapsedMs() const;
    bool isRunning() const { return running_; }

private:
    std::chrono::high_resolution_clock::time_point start_time_;
    std::chrono::high_resolution_clock::time_point end_time_;
    bool running_;
};

// Profile entry for a single measurement
struct ProfileEntry {
    std::string name;
    std::string type;  // "function" or "block"
    double duration_ms;
    std::chrono::system_clock::time_point timestamp;
};

// Statistics for a profiled item
struct ProfileStats {
    std::string name;
    std::string type;
    int call_count;
    double total_ms;
    double avg_ms;
    double min_ms;
    double max_ms;
};

// Complete profile report
struct ProfileReport {
    std::vector<ProfileStats> function_stats;
    std::vector<ProfileStats> block_stats;
    double total_time_ms;
    int total_entries;

    std::string toString() const;
};

// Performance Profiler - tracks execution statistics
class Profiler {
public:
    Profiler();
    ~Profiler() = default;

    // Enable/disable profiling
    void enable();
    void disable();
    bool isEnabled() const { return enabled_; }

    // Function profiling
    void startFunction(const std::string& name);
    void endFunction(const std::string& name);

    // Block profiling
    void startBlock(const std::string& block_id);
    void endBlock(const std::string& block_id);

    // Generate profile report
    ProfileReport generateReport() const;

    // Clear all profiling data
    void clear();

    // Get singleton instance
    static Profiler& instance();

private:
    bool enabled_;
    std::unordered_map<std::string, Timer> active_timers_;
    std::vector<ProfileEntry> entries_;

    // Singleton instance
    static Profiler* instance_;

    // Record an entry
    void recordEntry(const std::string& name, const std::string& type, double duration_ms);

    // Calculate statistics from entries
    ProfileStats calculateStats(const std::string& name, const std::string& type) const;
};

// RAII helper for automatic profiling
class ScopedProfile {
public:
    ScopedProfile(const std::string& name, const std::string& type = "function");
    ~ScopedProfile();

private:
    std::string name_;
    std::string type_;
    bool enabled_;
};

// Convenience macros
#define PROFILE_FUNCTION() naab::profiling::ScopedProfile __profile__(__func__, "function")
#define PROFILE_BLOCK(name) naab::profiling::ScopedProfile __profile__##name(#name, "block")

} // namespace profiling
} // namespace naab

#endif // NAAB_PROFILER_H
