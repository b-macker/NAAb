// Which VM a tool runs on (A93). See include/naab/thread_vm.h.
#include "naab/thread_vm.h"

namespace naab {
namespace runtime {

namespace {
thread_local ThreadVM t_thread_vm;
}

ThreadVM currentThreadVM() { return t_thread_vm; }

RunningVMScope::RunningVMScope(void* vm) : saved_(t_thread_vm) {
    // Entering a VM: its code runs here, not through a loan, so a tool called
    // from inside it is a nested call and takes no lock (the entry that
    // brought this worker in already holds the loan's).
    t_thread_vm = ThreadVM{vm, nullptr};
}

RunningVMScope::~RunningVMScope() { t_thread_vm = saved_; }

VMLoan::VMLoan() : state_(std::make_shared<VMLoanState>()) {
    state_->vm = t_thread_vm.vm;
}

VMLoan::~VMLoan() { state_->revoked.store(true, std::memory_order_release); }

ThreadVM VMLoan::lent() const {
    if (!state_->vm) return ThreadVM{};
    return ThreadVM{state_->vm, state_};
}

LentVMScope::LentVMScope(ThreadVM lent) : saved_(t_thread_vm) { t_thread_vm = std::move(lent); }

LentVMScope::~LentVMScope() { t_thread_vm = saved_; }

}  // namespace runtime
}  // namespace naab
