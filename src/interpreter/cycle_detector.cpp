// Phase 3.2: Cycle Detection Implementation
// Mark-and-sweep garbage collector for breaking reference cycles

#include "cycle_detector.h"
#include "naab/interpreter.h"
#include <fmt/core.h>

namespace naab {
namespace interpreter {

// Mark all values reachable from a given value
void CycleDetector::markReachable(
    std::shared_ptr<Value> value,
    std::set<std::shared_ptr<Value>>& visited,
    std::set<std::shared_ptr<Value>>& reachable)
{
    if (!value || visited.count(value)) {
        return;  // Null or already visited
    }

    visited.insert(value);
    reachable.insert(value);

    // Recursively mark all child values
    value->traverse([&](std::shared_ptr<Value> child) {
        markReachable(child, visited, reachable);
    });
}

// Mark all values reachable from environment variables
void CycleDetector::markFromEnvironment(
    std::shared_ptr<Environment> env,
    std::set<std::shared_ptr<Value>>& visited,
    std::set<std::shared_ptr<Value>>& reachable)
{
    if (!env) {
        return;
    }

    // Get all values in this environment
    const auto& values = env->getValues();

    for (const auto& [name, value] : values) {
        if (value) {
            markReachable(value, visited, reachable);
        }
    }

    // Recursively process parent environment
    auto parent = env->getParent();
    if (parent) {
        markFromEnvironment(parent, visited, reachable);
    }
}

// Find values that are in cycles (not reachable but have refcount > 1)
std::vector<std::shared_ptr<Value>> CycleDetector::findCycles(
    const std::set<std::shared_ptr<Value>>& reachable,
    const std::set<std::shared_ptr<Value>>& all_values)
{
    std::vector<std::shared_ptr<Value>> cycles;

    // In a real implementation, we would track all allocated values
    // For now, this is a simplified version that finds values
    // that have multiple references but aren't reachable from roots

    for (const auto& value : all_values) {
        // Skip null values
        if (!value) {
            continue;
        }

        // If value is not reachable but has refcount > 1, it's in a cycle
        if (reachable.find(value) == reachable.end()) {
            long refcount = value.use_count();
            if (refcount > 1) {  // 1 ref is from all_values set
                cycles.push_back(value);
            }
        }
    }

    return cycles;
}

// Break cycles by clearing internal references
void CycleDetector::breakCycles(const std::vector<std::shared_ptr<Value>>& cycles)
{
    for (const auto& value : cycles) {
        if (!value) {
            continue;
        }

        // Clear internal references to break the cycle
        // This will be done by setting fields to null/empty
        std::visit([](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;

            // Clear list elements
            if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<Value>>>) {
                arg.clear();
            }
            // Clear dict entries
            else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<Value>>>) {
                arg.clear();
            }
            // Clear struct fields
            else if constexpr (std::is_same_v<T, std::shared_ptr<StructValue>>) {
                if (arg) {
                    arg->field_values.clear();
                }
            }
        }, value->data);
    }

    last_collection_count_ = cycles.size();
    total_collected_ += cycles.size();
}

// Main entry point: detect and collect cycles (COMPLETE TRACING GC)
size_t CycleDetector::detectAndCollect(std::shared_ptr<Environment> root_env,
                                      std::vector<std::weak_ptr<Value>>& tracked_values,
                                      const std::vector<std::shared_ptr<Value>>& extra_roots,
                                      const std::vector<std::shared_ptr<Environment>>& extra_envs)
{
    if (!root_env) {
        return 0;
    }

    // Phase 1: Mark - find all reachable values from environment roots
    std::set<std::shared_ptr<Value>> visited;
    std::set<std::shared_ptr<Value>> reachable;

    // Mark all reachable values from the environment (includes parent chain)
    markFromEnvironment(root_env, visited, reachable);

    // Mark additional environments (e.g., global_env_ when root is current_env_)
    for (const auto& env : extra_envs) {
        if (env) {
            markFromEnvironment(env, visited, reachable);
        }
    }

    // Mark additional root values (e.g., result_, in-flight return values)
    for (const auto& val : extra_roots) {
        if (val) {
            markReachable(val, visited, reachable);
        }
    }

    // Phase 2: Build set of ALL tracked values (including out-of-scope)
    std::set<std::shared_ptr<Value>> all_values;

    // Convert tracked weak_ptrs to shared_ptrs, removing expired ones
    auto it = tracked_values.begin();
    while (it != tracked_values.end()) {
        if (auto value = it->lock()) {
            all_values.insert(value);
            ++it;
        } else {
            // Remove expired weak_ptr
            it = tracked_values.erase(it);
        }
    }

    fmt::print("[GC] Mark phase: {} values reachable (from {} total tracked)\n",
              reachable.size(), all_values.size());

    // Phase 3: Sweep - find unreachable cycles
    // These are values in all_values but NOT in reachable
    auto cycles = findCycles(reachable, all_values);

    fmt::print("[GC] Sweep phase: {} cycles detected\n", cycles.size());

    // Phase 4: Collect - break the cycles
    if (!cycles.empty()) {
        breakCycles(cycles);
        fmt::print("[GC] Collected {} cyclic values\n", cycles.size());
    }

    return cycles.size();
}

} // namespace interpreter
} // namespace naab
