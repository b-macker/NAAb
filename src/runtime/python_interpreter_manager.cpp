// Global Python Interpreter Manager Implementation

#include "naab/python_interpreter_manager.h"
#include "naab/python_c_wrapper.h"
#include <fmt/core.h>
#include <stdexcept>

namespace naab {
namespace runtime {

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

    if (python_c_init() != 0) {
        throw std::runtime_error("Failed to initialize Python interpreter");
    }
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
