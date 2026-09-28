// Global Python Interpreter Manager Implementation

#include "naab/python_interpreter_manager.h"
#include "naab/python_c_wrapper.h"
#include "naab/resource_limits.h"
#include <fmt/core.h>
#include <stdexcept>

// Defined in python_c_executor.cpp — the sandbox policy the audit hook calls.
extern "C" int naabPythonAuditPolicy(const char* event, const char* target, int is_write);

namespace naab {
namespace runtime {

// Runs on the timeout's timer thread: interrupt the Python that was running
// on the timed-out thread (or everywhere, for a script-wide deadline).
static void pythonTimeoutHook(unsigned long long thread_key, bool script_wide) {
    python_c_interrupt(static_cast<unsigned long>(thread_key), script_wide ? 1 : 0);
}

// Static member initialization
std::unique_ptr<PythonInterpreterManager> PythonInterpreterManager::instance_ = nullptr;
std::mutex PythonInterpreterManager::init_mutex_;
bool PythonInterpreterManager::initialized_ = false;

PythonInterpreterManager::PythonInterpreterManager()
{
    // Initialize global Python interpreter using pure C API
    // This approach:
    // - Calls Py_Initialize() to create global interpreter
    // - Calls PyEval_SaveThread() to release GIL for worker threads
    // - Allows worker threads to use PyGILState_Ensure/Release
    //
    // Benefits:
    // - 5x faster than pybind11 (3μs vs 15μs per call)
    // - No Android CFI crashes (bypasses bionic linker CFI issue)
    // - Thread-safe parallel Python execution

    // Give CPython the sandbox's call site before the interpreter exists.
    // Must precede python_c_init(): the hook is installed ahead of
    // Py_Initialize() and CPython guarantees audit hooks cannot be removed
    // afterwards, so executed block code cannot displace it the way it can
    // reassign builtins.__import__.
    python_c_set_audit_policy(&naabPythonAuditPolicy);

    if (python_c_init() != 0) {
        throw std::runtime_error("Failed to initialize Python interpreter");
    }

    // Let --timeout interrupt Python code: the timer thread queues a pending
    // call that raises TimeoutError inside the running block.
    python_c_set_timeout_check([]() -> int {
        return naab::security::ResourceLimiter::isTimeoutTriggered() ? 1 : 0;
    });
    naab::security::ResourceLimiter::setTimeoutInterruptHook(&pythonTimeoutHook);
}

PythonInterpreterManager::~PythonInterpreterManager() {
    // Shutdown Python using C API
    python_c_shutdown();
}

void PythonInterpreterManager::initialize() {
    std::lock_guard<std::mutex> lock(init_mutex_);

    if (initialized_) {
        // Interpreter already initialized (silent)
        return;
    }

    instance_ = std::unique_ptr<PythonInterpreterManager>(new PythonInterpreterManager());
    initialized_ = true;
}

bool PythonInterpreterManager::isInitialized() {
    std::lock_guard<std::mutex> lock(init_mutex_);
    return initialized_;
}

PythonInterpreterManager* PythonInterpreterManager::getInstance() {
    std::lock_guard<std::mutex> lock(init_mutex_);
    return instance_.get();
}

void PythonInterpreterManager::ensureInitialized() {
    if (!isInitialized()) {
        throw std::runtime_error(
            "Python interpreter not initialized. "
            "Call PythonInterpreterManager::initialize() from main thread first."
        );
    }
}

} // namespace runtime
} // namespace naab
