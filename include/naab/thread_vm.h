#pragma once
// Which VM a tool runs on (A93).
//
// A tool registered with agent.register_tool() is NAAb code, and a VM may be
// executed by one thread at a time. Every tool used to run on the MAIN VM,
// through a callback bound to it at startup, from whatever thread the tool
// loop happened to run on: two agent.batch workers pushed frames onto one
// stack at once (observed: a tool's own loop counter read a string, the
// batch's result read null afterwards), and a tool called from an async fn ran
// on main's stack while main was executing (observed: SIGSEGV, main's own
// values freed under it).
//
// A tool now runs on the VM that made the agent call:
//   - on that VM's own thread (agent.send in main, or in an async fn), as a
//     nested call -- the shape every native callback already has;
//   - on an agent.batch / fan_out worker, the dispatching VM, LENT to the
//     batch: its thread waits in agent.batch for the workers, and they enter
//     it one at a time. Each tool call keeps the dispatcher's frames below
//     it, so function-scope capabilities still intersect the caller's entry
//     (a fresh VM per tool would drop them, which loosens).
// A loan is revoked when the dispatcher leaves agent.batch by any path: a
// worker still in its tool loop afterwards (the dispatcher left on a HARD
// block) is refused instead of entering a VM that is unwinding. A worker
// already inside finishes that call; the dispatcher does not wait for it,
// so a long tool cannot turn a HARD block into a hang.
//
// Type-erased: the agent module cannot name the VM, which links it.
#include <atomic>
#include <memory>
#include <mutex>

namespace naab {
namespace runtime {

struct VMLoanState {
    void* vm = nullptr;
    std::mutex entry;               // held by a worker for each entry
    std::atomic<bool> revoked{false};
};

struct ThreadVM {
    void* vm = nullptr;                       // the VM NAAb code here runs on, or null
    std::shared_ptr<VMLoanState> loan;        // set on a worker the VM was lent to
};

// The VM a native call on this thread came from: the innermost VM executing
// on this thread, else the one lent to it, else none.
ThreadVM currentThreadVM();

// VM::run(): this thread is executing `vm` until the scope ends. Nested runs
// (a callback, a module, a lent VM entered) restore the outer one.
class RunningVMScope {
public:
    explicit RunningVMScope(void* vm);
    ~RunningVMScope();
    RunningVMScope(const RunningVMScope&) = delete;
    RunningVMScope& operator=(const RunningVMScope&) = delete;
private:
    ThreadVM saved_;
};

// agent.batch: lend the calling thread's VM to the workers it starts, for
// as long as this object lives. Revoked on destruction.
class VMLoan {
public:
    VMLoan();
    ~VMLoan();
    VMLoan(const VMLoan&) = delete;
    VMLoan& operator=(const VMLoan&) = delete;
    // What a worker installs (LentVMScope); null loan when no VM runs here.
    ThreadVM lent() const;
private:
    std::shared_ptr<VMLoanState> state_;
};

// A worker body: NAAb code on this thread runs on the lent VM.
class LentVMScope {
public:
    explicit LentVMScope(ThreadVM lent);
    ~LentVMScope();
    LentVMScope(const LentVMScope&) = delete;
    LentVMScope& operator=(const LentVMScope&) = delete;
private:
    ThreadVM saved_;
};

}  // namespace runtime
}  // namespace naab
