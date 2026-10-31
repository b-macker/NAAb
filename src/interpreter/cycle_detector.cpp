// Phase 3.2: Cycle Detection Implementation
// Mark-and-sweep garbage collector for breaking reference cycles
// Phase J: Fully NaabVal-based — iterates handle table directly, no toLegacy bridges

#include "cycle_detector.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <mutex>

namespace naab {
namespace interpreter {

// ---------------------------------------------------------------------------
// Engine activity (A92) -- see cycle_detector.h
// ---------------------------------------------------------------------------
namespace {
std::mutex g_activity_mutex;
// Activations on every thread plus pending engines; guarded by g_activity_mutex.
int g_activity = 0;

struct ThreadActivation {
    const void* engine;
    int depth;
};
// This thread's activations, innermost last. Written only by this thread, under
// g_activity_mutex when a record is pushed or popped.
thread_local std::vector<ThreadActivation> t_activations;
}  // namespace

EngineActivation::EngineActivation(const void* engine) {
    if (!t_activations.empty() && t_activations.back().engine == engine) {
        ++t_activations.back().depth;  // nested on the same engine and thread
        return;
    }
    std::lock_guard<std::mutex> lock(g_activity_mutex);
    t_activations.push_back({engine, 1});
    ++g_activity;
    counted_ = true;
}

EngineActivation::~EngineActivation() {
    if (t_activations.empty()) return;  // unreachable: activations are scoped
    if (!counted_) {
        --t_activations.back().depth;
        return;
    }
    std::lock_guard<std::mutex> lock(g_activity_mutex);
    t_activations.pop_back();
    --g_activity;
}

PendingEngine::PendingEngine() {
    std::lock_guard<std::mutex> lock(g_activity_mutex);
    ++g_activity;
}

void PendingEngine::release() {
    if (!held_) return;
    held_ = false;
    std::lock_guard<std::mutex> lock(g_activity_mutex);
    --g_activity;
}

PendingEngine::~PendingEngine() { release(); }

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
    // A92: collect only when this engine is the only activity in the process.
    // Its roots are its own; a value another engine is using is not among them
    // and can look like cycle garbage (see cycle_detector.h). try_lock, so a
    // collector never waits: a collection that cannot run now runs at a later
    // trigger. The lock is held to the end, so no engine starts or stops while
    // the sweep runs.
    std::unique_lock<std::mutex> gate(g_activity_mutex, std::try_to_lock);
    const bool alone = gate.owns_lock() &&
        t_activations.size() <= 1 &&
        g_activity == static_cast<int>(t_activations.size()) &&
        (t_activations.empty() || t_activations.back().engine == this);
    if (!alone) {
        ++collections_deferred_;
        last_collection_count_ = 0;
        return 0;
    }
    ++collections_run_;

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
