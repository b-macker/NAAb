// Global Python Interpreter Manager Implementation

#include "naab/python_interpreter_manager.h"
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
    fmt::print("[Python] Initializing global Python interpreter...\n");
    interpreter_ = std::make_unique<py::scoped_interpreter>();

    // CRITICAL: Release the GIL so other threads can acquire it
    // py::scoped_interpreter acquires GIL on creation and holds it
    // Use py::gil_scoped_release to properly release it for worker threads
    gil_release_ = std::make_unique<py::gil_scoped_release>();

    fmt::print("[Python] Global interpreter initialized and GIL released for multi-threading\n");
}

PythonInterpreterManager::~PythonInterpreterManager() {
    fmt::print("[Python] Shutting down global Python interpreter\n");

    // Re-acquire GIL before destroying the interpreter
    // Destroy gil_release_ which re-acquires the GIL
    gil_release_.reset();

    // Now safe to destroy interpreter (it expects to hold GIL)
    interpreter_.reset();
}

void PythonInterpreterManager::initialize() {
    std::lock_guard<std::mutex> lock(init_mutex_);

    if (initialized_) {
        fmt::print("[Python] Interpreter already initialized (no-op)\n");
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

// PythonGILGuard implementation
PythonGILGuard::PythonGILGuard()
    : gil_() // Acquires GIL
{
    PythonInterpreterManager::ensureInitialized();
}

} // namespace runtime
} // namespace naab
