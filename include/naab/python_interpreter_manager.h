#ifndef NAAB_PYTHON_INTERPRETER_MANAGER_H
#define NAAB_PYTHON_INTERPRETER_MANAGER_H

// Global Python Interpreter Manager
// Ensures Python interpreter is initialized once and accessible from all threads
// Uses pure C API (python_c_wrapper.c) - NO pybind11!

#include <mutex>
#include <memory>

namespace naab {
namespace runtime {

/**
 * @brief Singleton manager for the global Python interpreter.
 *
 * Uses pure Python C API (python_c_wrapper.c) for initialization.
 * Calls python_c_init() which does Py_Initialize() + PyEval_SaveThread().
 * Thread-safe - worker threads use PyGILState_Ensure/Release.
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
     * @brief Verify Python interpreter is initialized.
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

    // NOTE: Using pure C API (python_c_wrapper.c) instead of pybind11
    // No member variables needed - python_c_init/shutdown handle everything
};

} // namespace runtime
} // namespace naab

#endif // NAAB_PYTHON_INTERPRETER_MANAGER_H
