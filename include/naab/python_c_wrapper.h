#pragma once
/**
 * Pure C Wrapper for Python Execution
 *
 * Provides thread-safe Python execution from any thread.
 * Uses pre-created PyThreadState + PyEval_RestoreThread/SaveThread
 * to avoid PyGILState_Ensure which crashes on Android (bionic CFI).
 *
 * No pybind11 overhead - uses raw Python C API for 5x better performance.
 */


#ifdef HAVE_PYBIND11
#include <Python.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Execution result structure
 */
typedef struct {
    int success;           // 1 = success, 0 = error
    char* error_message;   // Error message (NULL if success)
#ifdef HAVE_PYBIND11
    PyObject* result;      // Python result object (NULL if error or void)
#else
    void* result;          // Opaque when Python is unavailable
#endif
} PythonCResult;

/**
 * Initialize Python interpreter (call once from main thread)
 *
 * This function:
 * - Initializes Python via Py_Initialize()
 * - Releases the GIL via PyEval_SaveThread()
 * - Saves main thread state for later GIL re-acquisition
 *
 * Returns: 0 on success, -1 on error
 */
int python_c_init(void);

/**
 * Audit policy callback — the sandbox's call site inside CPython.
 *
 * Every other I/O path in NAAb asks the sandbox directly (file.read ->
 * canRead, http.get -> canConnect, process.run -> canExecuteCommand). A
 * polyglot block is the one path with no NAAb call site: once control enters
 * CPython there is nowhere to put the question, which is why polyglot
 * governance has been source-text-only and why an object-graph traversal
 * (reaching an already-cached module without importing) escaped it.
 *
 * PySys_AddAuditHook is CPython's own embedder hook. It fires at the moment
 * of the operation regardless of how the module reference was obtained, so it
 * supplies the missing call site. This wrapper handles the Python-side
 * mechanics (decoding audit args, and recognising the interpreter loading its
 * own modules); the policy decision belongs to C++, which owns the sandbox.
 *
 * Returns 1 to allow, 0 to deny. `target` is the primary argument for the
 * event (path, command, or host) and may be NULL. `is_write` is 1 when the
 * event carries write intent.
 *
 * Must be set before python_c_init(); the hook is installed prior to
 * Py_Initialize() and, per CPython's guarantee, can never be removed
 * afterwards — unlike the builtins.__import__ override, which the executed
 * block can reassign.
 */
typedef int (*NaabPyAuditPolicyFn)(const char* event, const char* target, int is_write);
void python_c_set_audit_policy(NaabPyAuditPolicyFn fn);

/**
 * Create a Python thread state for the current thread.
 *
 * Safe to call from any thread WITHOUT the GIL.
 * Uses PyThreadState_New() which has its own internal lock.
 * The thread state will have the correct thread_id for the calling thread.
 *
 * Call this from each worker thread at startup, then pass the result
 * to python_c_set_thread_state().
 *
 * @return Opaque handle (void* wrapping PyThreadState*). NULL on error.
 */
void* python_c_create_thread_state(void);

/**
 * Register a pre-created thread state for the current thread.
 *
 * After calling this, python_c_gil_acquire/release will use the
 * pre-created state instead of PyGILState_Ensure (which crashes on Android).
 *
 * @param tstate Handle from python_c_create_thread_state()
 */
void python_c_set_thread_state(void* tstate);

/**
 * Destroy a pre-created thread state.
 *
 * Call when the worker thread is about to exit.
 * Acquires GIL internally to safely clean up.
 *
 * @param tstate Handle from python_c_create_thread_state()
 */
void python_c_destroy_thread_state(void* tstate);

/**
 * Acquire the GIL safely from any thread.
 *
 * On worker threads (with pre-created state): uses PyEval_RestoreThread
 * On main/unregistered threads: uses PyGILState_Ensure
 *
 * This avoids the Android bionic CFI crash that occurs with
 * repeated PyGILState_Ensure calls from thread pool workers.
 *
 * @return Handle to pass to python_c_gil_release(). Opaque int.
 */
int python_c_gil_acquire(void);

/**
 * Release the GIL safely. Companion to python_c_gil_acquire().
 *
 * @param handle Value returned by python_c_gil_acquire()
 */
void python_c_gil_release(int handle);

/**
 * Execute Python code from ANY thread (thread-safe)
 *
 * Uses python_c_gil_acquire/release internally for safe GIL management.
 *
 * @param code Python code to execute
 * @return PythonCResult containing success/error and result
 *
 * IMPORTANT: Caller must free result with python_c_free_result()
 */
PythonCResult python_c_execute(const char* code);

/**
 * Execute Python expression and return result (thread-safe)
 *
 * Uses python_c_gil_acquire/release internally for safe GIL management.
 *
 * @param code Python expression to evaluate
 * @return PythonCResult containing success/error and result
 *
 * IMPORTANT: Caller must free result with python_c_free_result()
 */
PythonCResult python_c_eval(const char* code);

/**
 * Free PythonCResult resources
 *
 * @param result Result to free
 */
void python_c_free_result(PythonCResult* result);

/**
 * Convert PyObject to string representation
 *
 * @param obj PyObject to convert
 * @return String representation (caller must free with free())
 */
#ifdef HAVE_PYBIND11
char* python_c_object_to_string(PyObject* obj);
#else
char* python_c_object_to_string(void* obj);
#endif

/**
 * Warm up Python C API from worker thread.
 *
 * Must be called AFTER python_c_set_thread_state().
 * Acquires GIL and exercises all Python C API functions that will be
 * used later (PyRun_String, type conversion, list/dict operations).
 *
 * Purpose: On Android, bionic's CFI (Control Flow Integrity) allocates
 * shadow memory via mmap() the first time each function pointer target
 * is called. If this happens LATE (after address space is fragmented
 * by QuickJS/other allocations), mmap() fails and the process crashes.
 * By warming up ALL Python functions EARLY during worker startup, we
 * ensure CFI shadow entries exist before address space fills up.
 */
void python_c_warmup(void);

/**
 * Shutdown Python interpreter (call once from main thread)
 *
 * Returns: 0 on success, -1 on error
 */
int python_c_shutdown(void);

#ifdef __cplusplus
}
#endif

