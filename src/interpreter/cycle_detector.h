#pragma once

// Phase 3.2: Cycle Detection and Garbage Collection
// Mark-and-sweep algorithm for detecting and breaking reference cycles
// Phase J: Fully NaabVal-based — no shared_ptr<Value> bridges

#include "naab/naab_val.h"
#include <memory>
#include <unordered_set>
#include <vector>
#include <functional>

namespace naab {
namespace interpreter {

// Forward declarations
class Environment;

class CycleDetector {
public:
    CycleDetector() = default;

    // Run cycle detection and collection
    // Returns number of values collected
    // extra_roots: additional NaabVal values to mark as reachable (e.g., result_)
    // extra_envs: additional environments to mark from (e.g., global_env_)
    size_t detectAndCollect(std::shared_ptr<Environment> root_env,
                           const std::vector<NaabVal>& extra_roots = {},
                           const std::vector<std::shared_ptr<Environment>>& extra_envs = {});

    // Get statistics
    size_t getTotalAllocations() const { return total_allocations_; }
    size_t getTotalCollected() const { return total_collected_; }
    size_t getLastCollectionCount() const { return last_collection_count_; }

private:
    // Mark phase: recursively mark all reachable NaabVal values from a root
    void markReachable(const NaabVal& value,
                      std::unordered_set<uint64_t>& visited);

    // Mark all values reachable from environment
    void markFromEnvironment(std::shared_ptr<Environment> env,
                            std::unordered_set<uint64_t>& visited);

    // Break cycles by clearing containers in unreachable values
    size_t breakUnreachable(const std::unordered_set<uint64_t>& reachable);

    // Statistics
    size_t total_allocations_ = 0;
    size_t total_collected_ = 0;
    size_t last_collection_count_ = 0;
};

} // namespace interpreter
} // namespace naab
