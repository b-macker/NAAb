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
    // Value::traverse now passes NaabVal — convert to shared_ptr for marking
    value->traverse([&](const NaabVal& child) {
        if (child.isHeap()) {
            markReachable(child.toLegacy(), visited, reachable);
        }
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

    // Get all values in this environment (NaabVal)
    const auto& values = env->getValues();

    for (const auto& [name, nval] : values) {
        // Traverse NaabVal to reach contained values for marking
        nval.traverse([&](const NaabVal& child) {
            if (child.isHeap()) {
                markReachable(child.toLegacy(), visited, reachable);
            }
        });
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

    for (const auto& value : all_values) {
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

        std::visit([](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;

            if constexpr (std::is_same_v<T, std::vector<NaabVal>>) {
                arg.clear();
            }
            else if constexpr (std::is_same_v<T, std::unordered_map<std::string, NaabVal>>) {
                arg.clear();
            }
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
                                      const std::vector<NaabVal>& extra_roots,
                                      const std::vector<std::shared_ptr<Environment>>& extra_envs)
{
    if (!root_env) {
        return 0;
    }

    // Phase 1: Mark - find all reachable values from environment roots
    std::set<std::shared_ptr<Value>> visited;
    std::set<std::shared_ptr<Value>> reachable;

    markFromEnvironment(root_env, visited, reachable);

    // Mark additional environments
    for (const auto& env : extra_envs) {
        if (env) {
            markFromEnvironment(env, visited, reachable);
        }
    }

    // Mark additional root values (NaabVal — convert to shared_ptr internally)
    for (const auto& nval : extra_roots) {
        if (nval.isHeap()) {
            markReachable(nval.toLegacy(), visited, reachable);
        }
    }

    // Phase 2: Build set of ALL tracked values
    std::set<std::shared_ptr<Value>> all_values;

    auto it = tracked_values.begin();
    while (it != tracked_values.end()) {
        if (auto value = it->lock()) {
            all_values.insert(value);
            ++it;
        } else {
            it = tracked_values.erase(it);
        }
    }

    // Phase 3: Sweep - find unreachable cycles
    auto cycles = findCycles(reachable, all_values);

    // Phase 4: Collect - break the cycles
    if (!cycles.empty()) {
        breakCycles(cycles);
    }

    return cycles.size();
}

} // namespace interpreter
} // namespace naab
