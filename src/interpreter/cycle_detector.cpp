// Phase 3.2: Cycle Detection Implementation
// Mark-and-sweep garbage collector for breaking reference cycles
// Phase J: Fully NaabVal-based — iterates handle table directly, no toLegacy bridges

#include "cycle_detector.h"
#include "naab/interpreter.h"
#include <fmt/core.h>

namespace naab {
namespace interpreter {

// Mark all values reachable from a given NaabVal
void CycleDetector::markReachable(
    const NaabVal& value,
    std::unordered_set<uint64_t>& visited)
{
    if (!value.isHeap()) {
        return;  // Inline types (int, double, bool, null) don't need GC
    }

    uint64_t bits = value.rawBits();
    if (visited.count(bits)) {
        return;  // Already visited
    }

    visited.insert(bits);

    // Recursively mark all child values via NaabVal::traverse
    value.traverse([&](const NaabVal& child) {
        markReachable(child, visited);
    });
}

// Mark all values reachable from environment variables
void CycleDetector::markFromEnvironment(
    std::shared_ptr<Environment> env,
    std::unordered_set<uint64_t>& visited)
{
    if (!env) {
        return;
    }

    // Get all values in this environment (already NaabVal)
    const auto& values = env->getValues();

    for (const auto& [name, nval] : values) {
        markReachable(nval, visited);
    }

    // Recursively process parent environment
    auto parent = env->getParent();
    if (parent) {
        markFromEnvironment(parent, visited);
    }
}

// Break cycles: iterate handle table, clear containers in unreachable values
// Only clear values with real refcount > 1 (indicating cycles, not stack refs).
// During forEachHeapValue callback, refcount has +2 overhead (iteration + copy).
size_t CycleDetector::breakUnreachable(const std::unordered_set<uint64_t>& reachable) {
    size_t collected = 0;

    NaabVal::forEachHeapValue([&](NaabVal val) {
        uint64_t bits = val.rawBits();
        if (reachable.count(bits)) {
            return;  // Reachable — keep alive
        }

        // Check real refcount: current refcount - 2 (iteration bump + callback copy)
        // Only break cycles (real refcount > 1). Values with real refcount == 1
        // are held by C++ locals and will be naturally freed.
        int real_refcount = val.getHeapRefCount() - 2;
        if (real_refcount <= 1) {
            return;  // Not a cycle — single reference from stack/temporary
        }

        // Unreachable with multiple references — cycle detected, clear container
        if (val.isList()) {
            val.asList().clear();
            collected++;
        } else if (val.isDict()) {
            val.asDict().clear();
            collected++;
        } else if (val.isStructVal()) {
            auto& s = val.asStruct();
            if (s) {
                s->field_values.clear();
                collected++;
            }
        }
    });

    return collected;
}

// Main entry point: detect and collect cycles
size_t CycleDetector::detectAndCollect(
    std::shared_ptr<Environment> root_env,
    const std::vector<NaabVal>& extra_roots,
    const std::vector<std::shared_ptr<Environment>>& extra_envs)
{
    // V-RT-008: allow null root_env for VM caller (VM uses flat stack, not Environment).
    // If root_env is null, skip env traversal — use extra_roots and extra_envs only.
    std::unordered_set<uint64_t> reachable;

    if (root_env) {
        markFromEnvironment(root_env, reachable);
    }

    // Mark additional environments (e.g., env_stack_ entries)
    for (const auto& env : extra_envs) {
        if (env) {
            markFromEnvironment(env, reachable);
        }
    }

    // Mark additional root values (e.g., result_, temporary values)
    for (const auto& nval : extra_roots) {
        markReachable(nval, reachable);
    }

    // Phase 2: Sweep — iterate handle table, break unreachable cycles
    size_t collected = breakUnreachable(reachable);

    last_collection_count_ = collected;
    total_collected_ += collected;

    return collected;
}

} // namespace interpreter
} // namespace naab
