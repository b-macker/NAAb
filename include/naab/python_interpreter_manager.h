#ifndef NAAB_PYTHON_INTERPRETER_MANAGER_H
#define NAAB_PYTHON_INTERPRETER_MANAGER_H

// Global Python Interpreter Manager
// Ensures Python interpreter is initialized once and accessible from all threads

#include <pybind11/pybind11.h>
#include <pybind11/embed.h>
#include <mutex>
#include <memory>

namespace py = pybind11;

namespace naab {
namespace runtime {

/**
 * @brief Singleton manager for the global Python interpreter.
 *
 * Ensures py::scoped_interpreter is initialized exactly once per process
 * from the main thread. Provides thread-safe access to Python functionality.
 */
class PythonInterpreterManager {
public:
    /**
     * @brief Initialize the global Python interpreter.
     *
     * MUST be called from the main thread before any Python operations.
     * Safe to call multiple times (subsequent calls are no-ops).
     */
    static void initialize();

    /**
     * @brief Check if Python interpreter is initialized.
     */
    static bool isInitialized();

    /**
     * @brief Get the singleton instance.
     *
     * Returns nullptr if not initialized. Always call initialize() first.
     */
    static PythonInterpreterManager* getInstance();

    /**
     * @brief Acquire the GIL for the current thread.
     *
     * Use py::gil_scoped_acquire in your code, but this verifies initialization.
     *
     * @throws std::runtime_error if interpreter not initialized
     */
    static void ensureInitialized();

    // Non-copyable, non-movable
    PythonInterpreterManager(const PythonInterpreterManager&) = delete;
    PythonInterpreterManager& operator=(const PythonInterpreterManager&) = delete;
    PythonInterpreterManager(PythonInterpreterManager&&) = delete;
    PythonInterpreterManager& operator=(PythonInterpreterManager&&) = delete;

    // Destructor needs to be public for unique_ptr
    ~PythonInterpreterManager();

private:
    PythonInterpreterManager();

    static std::unique_ptr<PythonInterpreterManager> instance_;
    static std::mutex init_mutex_;
    static bool initialized_;

    std::unique_ptr<py::scoped_interpreter> interpreter_;
    std::unique_ptr<py::gil_scoped_release> gil_release_;  // Releases GIL for multi-threading
};

/**
 * @brief RAII helper to acquire GIL with initialization check.
 *
 * Usage:
 *   PythonGILGuard guard;
 *   // Python code here
 */
class PythonGILGuard {
public:
    PythonGILGuard();
    ~PythonGILGuard() = default;

private:
    py::gil_scoped_acquire gil_;
};

} // namespace runtime
} // namespace naab

#endif // NAAB_PYTHON_INTERPRETER_MANAGER_H
