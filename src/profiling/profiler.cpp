// NAAb Performance Profiler Implementation
// Tracks and reports execution statistics

#include "naab/profiler.h"
#include <fmt/core.h>
#include <algorithm>
#include <limits>
#include <sstream>

namespace naab {
namespace profiling {

// Initialize singleton
Profiler* Profiler::instance_ = nullptr;

//=============================================================================
// Timer Implementation
//=============================================================================

Timer::Timer() : running_(false) {
}

void Timer::start() {
    start_time_ = std::chrono::high_resolution_clock::now();
    running_ = true;
}

void Timer::stop() {
    end_time_ = std::chrono::high_resolution_clock::now();
    running_ = false;
}

void Timer::reset() {
    running_ = false;
}

double Timer::elapsedMs() const {
    if (running_) {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_time_).count();
    } else {
        return std::chrono::duration<double, std::milli>(end_time_ - start_time_).count();
    }
}

//=============================================================================
// ProfileReport Implementation
//=============================================================================

std::string ProfileReport::toString() const {
    std::ostringstream oss;

    oss << "=== Performance Profile Report ===\n\n";

    // Function statistics
    if (!function_stats.empty()) {
        oss << "Function Calls:\n";
        for (const auto& stat : function_stats) {
            oss << fmt::format("  {}: {:.2f}ms ({} calls, avg: {:.2f}ms, min: {:.2f}ms, max: {:.2f}ms)\n",
                               stat.name, stat.total_ms, stat.call_count,
                               stat.avg_ms, stat.min_ms, stat.max_ms);
        }
        oss << "\n";
    }

    // Block statistics
    if (!block_stats.empty()) {
        oss << "Block Loading:\n";
        for (const auto& stat : block_stats) {
            oss << fmt::format("  {}: {:.2f}ms ({} loads, avg: {:.2f}ms)\n",
                               stat.name, stat.total_ms, stat.call_count, stat.avg_ms);
        }
        oss << "\n";
    }

    // Summary
    oss << fmt::format("Total Time: {:.2f}ms\n", total_time_ms);
    oss << fmt::format("Total Entries: {}\n", total_entries);

    return oss.str();
}

//=============================================================================
// Profiler Implementation
//=============================================================================

Profiler::Profiler() : enabled_(false) {
}

void Profiler::enable() {
    enabled_ = true;
    fmt::print("[PROFILER] Profiling enabled\n");
}

void Profiler::disable() {
    enabled_ = false;
    fmt::print("[PROFILER] Profiling disabled\n");
}

void Profiler::startFunction(const std::string& name) {
    if (!enabled_) return;

    auto& timer = active_timers_[name];
    timer.start();
}

void Profiler::endFunction(const std::string& name) {
    if (!enabled_) return;

    auto it = active_timers_.find(name);
    if (it == active_timers_.end()) {
        fmt::print("[WARN] endFunction called for '{}' without startFunction\n", name);
        return;
    }

    it->second.stop();
    double duration = it->second.elapsedMs();

    recordEntry(name, "function", duration);

    // Remove from active timers
    active_timers_.erase(it);
}

void Profiler::startBlock(const std::string& block_id) {
    if (!enabled_) return;

    auto& timer = active_timers_[block_id];
    timer.start();
}

void Profiler::endBlock(const std::string& block_id) {
    if (!enabled_) return;

    auto it = active_timers_.find(block_id);
    if (it == active_timers_.end()) {
        fmt::print("[WARN] endBlock called for '{}' without startBlock\n", block_id);
        return;
    }

    it->second.stop();
    double duration = it->second.elapsedMs();

    recordEntry(block_id, "block", duration);

    // Remove from active timers
    active_timers_.erase(it);
}

void Profiler::recordEntry(const std::string& name, const std::string& type, double duration_ms) {
    ProfileEntry entry;
    entry.name = name;
    entry.type = type;
    entry.duration_ms = duration_ms;
    entry.timestamp = std::chrono::system_clock::now();

    entries_.push_back(entry);
}

ProfileStats Profiler::calculateStats(const std::string& name, const std::string& type) const {
    ProfileStats stats;
    stats.name = name;
    stats.type = type;
    stats.call_count = 0;
    stats.total_ms = 0.0;
    stats.avg_ms = 0.0;
    stats.min_ms = std::numeric_limits<double>::max();
    stats.max_ms = 0.0;

    // Collect all entries for this name/type
    for (const auto& entry : entries_) {
        if (entry.name == name && entry.type == type) {
            stats.call_count++;
            stats.total_ms += entry.duration_ms;
            stats.min_ms = std::min(stats.min_ms, entry.duration_ms);
            stats.max_ms = std::max(stats.max_ms, entry.duration_ms);
        }
    }

    if (stats.call_count > 0) {
        stats.avg_ms = stats.total_ms / stats.call_count;
    }

    return stats;
}

ProfileReport Profiler::generateReport() const {
    ProfileReport report;
    report.total_entries = entries_.size();
    report.total_time_ms = 0.0;

    // Collect unique names by type
    std::unordered_map<std::string, bool> function_names;
    std::unordered_map<std::string, bool> block_names;

    for (const auto& entry : entries_) {
        report.total_time_ms += entry.duration_ms;

        if (entry.type == "function") {
            function_names[entry.name] = true;
        } else if (entry.type == "block") {
            block_names[entry.name] = true;
        }
    }

    // Calculate statistics for each function
    for (const auto& pair : function_names) {
        ProfileStats stats = calculateStats(pair.first, "function");
        report.function_stats.push_back(stats);
    }

    // Calculate statistics for each block
    for (const auto& pair : block_names) {
        ProfileStats stats = calculateStats(pair.first, "block");
        report.block_stats.push_back(stats);
    }

    // Sort by total time (descending)
    auto sort_by_total = [](const ProfileStats& a, const ProfileStats& b) {
        return a.total_ms > b.total_ms;
    };

    std::sort(report.function_stats.begin(), report.function_stats.end(), sort_by_total);
    std::sort(report.block_stats.begin(), report.block_stats.end(), sort_by_total);

    return report;
}

void Profiler::clear() {
    entries_.clear();
    active_timers_.clear();
    fmt::print("[PROFILER] Profiling data cleared\n");
}

Profiler& Profiler::instance() {
    if (!instance_) {
        instance_ = new Profiler();
    }
    return *instance_;
}

//=============================================================================
// ScopedProfile Implementation
//=============================================================================

ScopedProfile::ScopedProfile(const std::string& name, const std::string& type)
    : name_(name), type_(type) {

    auto& profiler = Profiler::instance();
    enabled_ = profiler.isEnabled();

    if (enabled_) {
        if (type_ == "function") {
            profiler.startFunction(name_);
        } else if (type_ == "block") {
            profiler.startBlock(name_);
        }
    }
}

ScopedProfile::~ScopedProfile() {
    if (enabled_) {
        auto& profiler = Profiler::instance();
        if (type_ == "function") {
            profiler.endFunction(name_);
        } else if (type_ == "block") {
            profiler.endBlock(name_);
        }
    }
}

} // namespace profiling
} // namespace naab
