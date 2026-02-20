#ifndef NAAB_CYCLE_DETECTOR_H
#define NAAB_CYCLE_DETECTOR_H

// Phase 3.2: Cycle Detection and Garbage Collection
// Mark-and-sweep algorithm for detecting and breaking reference cycles

#include <memory>
#include <set>
#include <vector>
#include <functional>

namespace naab {
namespace interpreter {

// Forward declarations
class Value;
class Environment;

class CycleDetector {
public:
    CycleDetector() = default;

    // Run cycle detection and collection
    // Returns number of values collected
    // Now supports global value tracking for complete GC
    // extra_roots: additional values to mark as reachable (e.g., result_, return values)
    // extra_envs: additional environments to mark from (e.g., global_env_)
    size_t detectAndCollect(std::shared_ptr<Environment> root_env,
                           std::vector<std::weak_ptr<Value>>& tracked_values,
                           const std::vector<std::shared_ptr<Value>>& extra_roots = {},
                           const std::vector<std::shared_ptr<Environment>>& extra_envs = {});

    // Get statistics
    size_t getTotalAllocations() const { return total_allocations_; }
    size_t getTotalCollected() const { return total_collected_; }
    size_t getLastCollectionCount() const { return last_collection_count_; }

private:
    // Mark phase: recursively mark all reachable values from roots
    void markReachable(std::shared_ptr<Value> value,
                      std::set<std::shared_ptr<Value>>& visited,
                      std::set<std::shared_ptr<Value>>& reachable);

    // Mark all values reachable from environment
    void markFromEnvironment(std::shared_ptr<Environment> env,
                            std::set<std::shared_ptr<Value>>& visited,
                            std::set<std::shared_ptr<Value>>& reachable);

    // Sweep phase: identify unreachable values that are in cycles
    // (refcount > 0 but not reachable from roots)
    std::vector<std::shared_ptr<Value>> findCycles(
        const std::set<std::shared_ptr<Value>>& reachable,
        const std::set<std::shared_ptr<Value>>& all_values);

    // Break cycles by clearing internal references
    void breakCycles(const std::vector<std::shared_ptr<Value>>& cycles);

    // Statistics
    size_t total_allocations_ = 0;
    size_t total_collected_ = 0;
    size_t last_collection_count_ = 0;
};

} // namespace interpreter
} // namespace naab

#endif // NAAB_CYCLE_DETECTOR_H
