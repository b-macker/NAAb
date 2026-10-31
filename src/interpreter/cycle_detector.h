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

// Engine activity, the collector's precondition (A92).
//
// A collection marks from ONE engine's roots (a VM's stack and globals, or an
// interpreter's environments), then sweeps every heap value in the process and
// clears any unreachable list, dict or struct whose refcount is above 1, on the
// premise that nothing else holds it. Another engine executing at the same time
// breaks that premise: its stack copies and its own collection's root copies
// raise the refcount of values it is using, and none of them is reachable from
// the collector's roots. Two async fns, or main{} and one async fn, cleared each
// other's live lists that way.
//
// So an engine records that it is executing (EngineActivation, per engine per
// thread; a nested activation of the same engine on the same thread -- a sort
// comparator, a callback -- is free), an engine about to start on another thread
// is counted from the moment it is spawned (PendingEngine, which covers its
// globals and arguments before its own activation begins), and detectAndCollect()
// collects only when the collecting engine is the only activity in the process.
// Otherwise the collection is deferred to a later trigger: concurrency can delay
// a collection, never clear a live value. The gate's lock is held for the whole
// collection, so no activation can begin or end while one runs.
class EngineActivation {
public:
    explicit EngineActivation(const void* engine);
    ~EngineActivation();
    EngineActivation(const EngineActivation&) = delete;
    EngineActivation& operator=(const EngineActivation&) = delete;
private:
    bool counted_ = false;  // false: a nested activation of the engine already on top
};

class PendingEngine {
public:
    PendingEngine();
    ~PendingEngine();
    PendingEngine(PendingEngine&& other) noexcept : held_(other.held_) { other.held_ = false; }
    PendingEngine& operator=(PendingEngine&&) = delete;
    PendingEngine(const PendingEngine&) = delete;
    PendingEngine& operator=(const PendingEngine&) = delete;
    void release();  // idempotent
private:
    bool held_ = true;
};

class CycleDetector {
public:
    CycleDetector() = default;

    // Run cycle detection and collection
    // Returns number of values collected (0 when the collection was deferred)
    // extra_roots: additional NaabVal values to mark as reachable (e.g., result_)
    // extra_envs: additional environments to mark from (e.g., global_env_)
    // Collects only when this detector's engine is the only engine executing in
    // the process (see EngineActivation above); otherwise counts a deferral.
    size_t detectAndCollect(std::shared_ptr<Environment> root_env,
                           const std::vector<NaabVal>& extra_roots = {},
                           const std::vector<std::shared_ptr<Environment>>& extra_envs = {});

    // Get statistics
    size_t getTotalAllocations() const { return total_allocations_; }
    size_t getTotalCollected() const { return total_collected_; }
    size_t getLastCollectionCount() const { return last_collection_count_; }
    size_t getCollectionsRun() const { return collections_run_; }
    size_t getCollectionsDeferred() const { return collections_deferred_; }

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
    size_t collections_run_ = 0;
    size_t collections_deferred_ = 0;
};

} // namespace interpreter
} // namespace naab
