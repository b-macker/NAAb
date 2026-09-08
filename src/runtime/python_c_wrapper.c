/**
 * Pure C Wrapper for Python Execution - Implementation
 *
 * Thread-safe Python execution using pre-created PyThreadState objects.
 * Avoids PyGILState_Ensure which triggers Android bionic CFI crashes.
 *
 * GIL management strategy:
 * - Worker threads: PyThreadState_New() at startup, then
 *   PyEval_RestoreThread/SaveThread for GIL acquire/release
 * - Main thread: PyGILState_Ensure/Release (safe on main thread)
 */

#include "naab/python_c_wrapper.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef _WIN32
#  include <fcntl.h>   // O_ACCMODE / O_WRONLY for audit write-detection
#endif

// Global state
static int python_initialized = 0;
static PyThreadState* main_thread_state = NULL;

// Thread-local storage for pre-created worker thread states
// __thread is supported on Android/bionic (GCC/Clang extension)
static __thread PyThreadState* worker_tstate = NULL;

// Sentinel value: python_c_gil_acquire returns -1 when using pre-created state
#define GIL_HANDLE_PRECREATED (-1)

// --- Sandbox audit hook (see python_c_wrapper.h) ---------------------------
// Policy lives in C++ (it owns the sandbox); this side only decodes CPython's
// audit arguments and recognises the interpreter loading its own modules.
static NaabPyAuditPolicyFn audit_policy = NULL;

void python_c_set_audit_policy(NaabPyAuditPolicyFn fn) {
    audit_policy = fn;
}

// Cached module-search roots, resolved lazily on the first audit event that
// arrives after the interpreter is usable.
//
// This carve-out is load-bearing, not cosmetic. Measured: three imports raise
// 19 "open" events, 19 of 19 under the module search path. Reading the
// interpreter's own files is the implementation of "you may run Python", not
// an I/O capability the block exercised -- and under mode: enforce the sandbox
// upgrades to `standard`, which refuses absolute paths, so without this every
// block that imports anything would break.
#define NAAB_MAX_ROOTS 64
static char* runtime_roots[NAAB_MAX_ROOTS];
static int runtime_roots_n = 0;
static int runtime_roots_ready = 0;

static void add_root(const char* p) {
    if (!p || !*p || runtime_roots_n >= NAAB_MAX_ROOTS) return;
    for (int i = 0; i < runtime_roots_n; i++) {
        if (strcmp(runtime_roots[i], p) == 0) return;
    }
    runtime_roots[runtime_roots_n] = strdup(p);
    if (runtime_roots[runtime_roots_n]) runtime_roots_n++;
}

static void resolve_runtime_roots(void) {
    if (runtime_roots_ready) return;
    if (!Py_IsInitialized()) return;
    runtime_roots_ready = 1;  // one attempt; never retry from inside a hook

    PyObject* sp = PySys_GetObject("path");   // borrowed
    if (sp && PyList_Check(sp)) {
        Py_ssize_t n = PyList_Size(sp);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* it = PyList_GetItem(sp, i);  // borrowed
            if (it && PyUnicode_Check(it)) {
                const char* c = PyUnicode_AsUTF8(it);
                if (c && *c) add_root(c);
            }
        }
    }
    PyObject* px = PySys_GetObject("prefix");
    if (px && PyUnicode_Check(px)) add_root(PyUnicode_AsUTF8(px));
    PyObject* bx = PySys_GetObject("base_prefix");
    if (bx && PyUnicode_Check(bx)) add_root(PyUnicode_AsUTF8(bx));
}

static int is_runtime_path(const char* path) {
    if (!path || !*path) return 0;
    resolve_runtime_roots();
    for (int i = 0; i < runtime_roots_n; i++) {
        size_t rl = strlen(runtime_roots[i]);
        if (rl == 0) continue;
        if (strncmp(path, runtime_roots[i], rl) == 0) return 1;
    }
    return 0;
}

// Pull argument `idx` out of the audit args tuple as a C long. Returns
// have_val=0 when the slot is absent or not an integer (e.g. None).
static long audit_long(PyObject* args, Py_ssize_t idx, int* have_val) {
    *have_val = 0;
    if (!args || !PyTuple_Check(args)) return 0;
    if (PyTuple_Size(args) <= idx) return 0;
    PyObject* o = PyTuple_GetItem(args, idx);  // borrowed
    if (!o || !PyLong_Check(o)) return 0;
    long v = PyLong_AsLong(o);
    if (v == -1 && PyErr_Occurred()) { PyErr_Clear(); return 0; }
    *have_val = 1;
    return v;
}

// Pull argument `idx` out of the audit args tuple as a UTF-8 string.
static const char* audit_str(PyObject* args, Py_ssize_t idx) {
    if (!args || !PyTuple_Check(args)) return NULL;
    if (PyTuple_Size(args) <= idx) return NULL;
    PyObject* o = PyTuple_GetItem(args, idx);  // borrowed
    if (!o) return NULL;
    if (PyUnicode_Check(o)) return PyUnicode_AsUTF8(o);
    return NULL;
}

static int naab_audit_hook(const char* event, PyObject* args, void* userData) {
    (void)userData;
    if (!audit_policy || !event) return 0;

    const char* target = NULL;
    int is_write = 0;
    int relevant = 0;

    if (strcmp(event, "open") == 0) {
        // args: (path, mode, flags) -- path may be an int fd, which audit_str
        // returns NULL for; a bare fd carries no path to adjudicate.
        target = audit_str(args, 0);
        // builtin open() carries a mode STRING (arg 1); os.open() carries
        // mode=None and the O_* access mode in the integer flags (arg 2).
        // Measured: os.open(p, O_WRONLY|O_CREAT) audits ("open", (p, None, int)),
        // so reading only the mode string classified a low-level write as a
        // read. Inspect both.
        const char* mode = audit_str(args, 1);
        if (mode && (strchr(mode, 'w') || strchr(mode, 'a') ||
                     strchr(mode, '+') || strchr(mode, 'x'))) {
            is_write = 1;
        }
        int have_flags = 0;
        long flags = audit_long(args, 2, &have_flags);
        if (have_flags) {
#ifdef O_ACCMODE
            if ((flags & O_ACCMODE) != O_RDONLY) is_write = 1;   // O_WRONLY / O_RDWR
#else
            if ((flags & 3) != 0) is_write = 1;   // POSIX access-mode bits
#endif
#ifdef O_CREAT
            if (flags & O_CREAT) is_write = 1;
#endif
#ifdef O_TRUNC
            if (flags & O_TRUNC) is_write = 1;
#endif
#ifdef O_APPEND
            if (flags & O_APPEND) is_write = 1;
#endif
        }
        if (target) {
            // The interpreter loading its own modules is not block I/O.
            if (!is_write && is_runtime_path(target)) return 0;
            relevant = 1;
        }
    } else if (strcmp(event, "os.system") == 0) {
        target = audit_str(args, 0);
        relevant = 1;
    } else if (strcmp(event, "subprocess.Popen") == 0) {
        target = audit_str(args, 0);
        relevant = 1;
    } else if (strncmp(event, "os.exec", 7) == 0 ||
               strcmp(event, "os.posix_spawn") == 0) {
        target = audit_str(args, 0);
        relevant = 1;
    } else if (strcmp(event, "socket.connect") == 0) {
        relevant = 1;   // address is a tuple; policy decides on capability
    }

    if (!relevant) return 0;

    if (audit_policy(event, target, is_write)) return 0;

    PyErr_Format(PyExc_PermissionError,
                 "Governance: %s denied by sandbox policy%s%s",
                 event,
                 target ? ": " : "",
                 target ? target : "");
    return -1;
}

/**
 * Initialize Python interpreter
 */
int python_c_init(void) {
    if (python_initialized) {
        return 0;
    }

    // Install the sandbox audit hook BEFORE Py_Initialize so it observes
    // the interpreter from the start and cannot be displaced afterwards.
    PySys_AddAuditHook(naab_audit_hook, NULL);

    // Initialize Python
    Py_Initialize();

    // Initialize threading support (deprecated in 3.9+, but harmless)
    #if PY_VERSION_HEX < 0x03090000
    PyEval_InitThreads();
    #endif

    // Release the GIL and save the main thread state
    // This allows worker threads to create their own thread states
    main_thread_state = PyEval_SaveThread();

    python_initialized = 1;
    return 0;
}

/**
 * Create a Python thread state for the current thread.
 *
 * PyThreadState_New() is safe to call from any thread without the GIL.
 * It uses its own internal lock (HEAD_LOCK) for thread safety.
 * The thread_id will be set to the CALLING thread's ID (correct for workers).
 */
void* python_c_create_thread_state(void) {
    if (!python_initialized) {
        fprintf(stderr, "[PythonC] Cannot create thread state: not initialized\n");
        return NULL;
    }

    // Get the main interpreter state (safe without GIL - just returns a pointer)
    PyInterpreterState* interp = PyInterpreterState_Main();
    if (!interp) {
        fprintf(stderr, "[PythonC] Cannot create thread state: no main interpreter\n");
        return NULL;
    }

    // Create a new thread state for this thread
    // PyThreadState_New has its own internal lock, safe without GIL
    // Sets thread_id = PyThread_get_thread_ident() = this thread's ID
    PyThreadState* tstate = PyThreadState_New(interp);
    if (!tstate) {
        fprintf(stderr, "[PythonC] Failed to create thread state\n");
        return NULL;
    }

    return (void*)tstate;
}

/**
 * Register a pre-created thread state for the current thread (TLS)
 */
void python_c_set_thread_state(void* tstate) {
    worker_tstate = (PyThreadState*)tstate;
}

/**
 * Destroy a pre-created thread state
 */
void python_c_destroy_thread_state(void* tstate) {
    if (!tstate) return;

    PyThreadState* ts = (PyThreadState*)tstate;

    // Need GIL to clear thread state (may trigger Python object finalization)
    PyEval_RestoreThread(ts);
    PyThreadState_Clear(ts);

    // Release GIL and delete the thread state
    // PyThreadState_DeleteCurrent() does both atomically
    PyThreadState_DeleteCurrent();

    // Clear TLS if this was our state
    if (worker_tstate == ts) {
        worker_tstate = NULL;
    }

}

/**
 * Acquire the GIL safely from any thread.
 *
 * Worker threads (with pre-created state): PyEval_RestoreThread
 *   - No TLS lookup via Python's autoTSSkey
 *   - No potential PyThreadState_New allocation
 *   - Minimal code path: just swap tstate + acquire mutex
 *
 * Main/unregistered threads: PyGILState_Ensure (fallback)
 *
 * Returns GIL_HANDLE_PRECREATED (-1) for pre-created, or PyGILState_STATE for fallback
 */
int python_c_gil_acquire(void) {
    if (worker_tstate) {
        // Worker thread: use pre-created state
        // PyEval_RestoreThread sets tstate as current + acquires GIL mutex
        // This is a MUCH simpler code path than PyGILState_Ensure
        PyEval_RestoreThread(worker_tstate);
        return GIL_HANDLE_PRECREATED;
    } else {
        // Main thread or unregistered thread: use PyGILState
        // (safe on main thread, risky on workers - but workers have pre-created states)
        PyGILState_STATE gstate = PyGILState_Ensure();
        return (int)gstate;
    }
}

/**
 * Release the GIL safely. Companion to python_c_gil_acquire().
 */
void python_c_gil_release(int handle) {
    if (handle == GIL_HANDLE_PRECREATED) {
        // Worker thread: save thread state and release GIL
        // PyEval_SaveThread returns the current tstate and releases GIL
        worker_tstate = PyEval_SaveThread();
    } else {
        // Main thread: use PyGILState
        PyGILState_Release((PyGILState_STATE)handle);
    }
}

/**
 * Execute Python code (statement mode) - thread-safe
 */
PythonCResult python_c_execute(const char* code) {
    PythonCResult result = {0};

    if (!python_initialized) {
        result.success = 0;
        result.error_message = strdup("Python not initialized. Call python_c_init() first.");
        result.result = NULL;
        return result;
    }

    // Acquire GIL safely (pre-created state on workers, PyGILState on main)
    int gil_handle = python_c_gil_acquire();

    // Get __main__ module
    PyObject* main_module = PyImport_AddModule("__main__");
    if (!main_module) {
        result.success = 0;
        result.error_message = strdup("Failed to get __main__ module");
        result.result = NULL;
        python_c_gil_release(gil_handle);
        return result;
    }

    // Get global dict
    PyObject* globals = PyModule_GetDict(main_module);
    if (!globals) {
        result.success = 0;
        result.error_message = strdup("Failed to get globals dict");
        result.result = NULL;
        python_c_gil_release(gil_handle);
        return result;
    }

    // Execute code as file input (allows statements)
    PyObject* py_result = PyRun_String(code, Py_file_input, globals, globals);

    if (py_result == NULL) {
        // Error occurred
        PyObject *ptype, *pvalue, *ptraceback;
        PyErr_Fetch(&ptype, &pvalue, &ptraceback);

        if (pvalue) {
            PyObject* str_exc = PyObject_Str(pvalue);
            if (str_exc) {
                const char* error_str = PyUnicode_AsUTF8(str_exc);
                result.error_message = strdup(error_str ? error_str : "Unknown Python error");
                Py_DECREF(str_exc);
            } else {
                result.error_message = strdup("Unknown Python error");
            }
        } else {
            result.error_message = strdup("Unknown Python error");
        }

        Py_XDECREF(ptype);
        Py_XDECREF(pvalue);
        Py_XDECREF(ptraceback);

        result.success = 0;
        result.result = NULL;
    } else {
        // Success
        result.success = 1;
        result.error_message = NULL;
        result.result = py_result;  // Transfer ownership
    }

    // Release GIL
    python_c_gil_release(gil_handle);

    return result;
}

/**
 * Execute Python expression (eval mode) - thread-safe
 */
PythonCResult python_c_eval(const char* code) {
    PythonCResult result = {0};

    if (!python_initialized) {
        result.success = 0;
        result.error_message = strdup("Python not initialized. Call python_c_init() first.");
        result.result = NULL;
        return result;
    }

    // Acquire GIL safely
    int gil_handle = python_c_gil_acquire();

    // Get __main__ module
    PyObject* main_module = PyImport_AddModule("__main__");
    if (!main_module) {
        result.success = 0;
        result.error_message = strdup("Failed to get __main__ module");
        result.result = NULL;
        python_c_gil_release(gil_handle);
        return result;
    }

    // Get global dict
    PyObject* globals = PyModule_GetDict(main_module);
    if (!globals) {
        result.success = 0;
        result.error_message = strdup("Failed to get globals dict");
        result.result = NULL;
        python_c_gil_release(gil_handle);
        return result;
    }

    // Evaluate expression
    PyObject* py_result = PyRun_String(code, Py_eval_input, globals, globals);

    if (py_result == NULL) {
        // Error occurred
        PyObject *ptype, *pvalue, *ptraceback;
        PyErr_Fetch(&ptype, &pvalue, &ptraceback);

        if (pvalue) {
            PyObject* str_exc = PyObject_Str(pvalue);
            if (str_exc) {
                const char* error_str = PyUnicode_AsUTF8(str_exc);
                result.error_message = strdup(error_str ? error_str : "Unknown Python error");
                Py_DECREF(str_exc);
            } else {
                result.error_message = strdup("Unknown Python error");
            }
        } else {
            result.error_message = strdup("Unknown Python error");
        }

        Py_XDECREF(ptype);
        Py_XDECREF(pvalue);
        Py_XDECREF(ptraceback);

        result.success = 0;
        result.result = NULL;
    } else {
        // Success
        result.success = 1;
        result.error_message = NULL;
        result.result = py_result;  // Transfer ownership
    }

    // Release GIL
    python_c_gil_release(gil_handle);

    return result;
}

/**
 * Convert PyObject to string
 */
char* python_c_object_to_string(PyObject* obj) {
    if (!obj) {
        return strdup("(null)");
    }

    // Acquire GIL safely
    int gil_handle = python_c_gil_acquire();

    PyObject* str = PyObject_Str(obj);
    char* result = NULL;

    if (str) {
        const char* utf8 = PyUnicode_AsUTF8(str);
        result = strdup(utf8 ? utf8 : "(error)");
        Py_DECREF(str);
    } else {
        result = strdup("(error)");
    }

    python_c_gil_release(gil_handle);

    return result;
}

/**
 * Free PythonCResult
 */
void python_c_free_result(PythonCResult* result) {
    if (!result) return;

    if (result->error_message) {
        free(result->error_message);
        result->error_message = NULL;
    }

    if (result->result) {
        // Acquire GIL safely to decref Python object
        int gil_handle = python_c_gil_acquire();
        Py_DECREF(result->result);
        python_c_gil_release(gil_handle);
        result->result = NULL;
    }
}

/**
 * Warm up Python C API from worker thread.
 *
 * Exercises all Python C API functions that will be used during actual
 * polyglot execution. This creates CFI shadow entries in Android's bionic
 * linker EARLY (before address space gets fragmented by QuickJS etc.).
 *
 * Must be called after python_c_set_thread_state() and with GIL NOT held.
 */
void python_c_warmup(void) {
    // === CRITICAL: Warm up libc functions FIRST ===
    // On Android, bionic's CFI allocates shadow memory (via mmap) the first
    // time each function pointer target is called from a shared library.
    // We must exercise all libc functions that Python's .so will use
    // BEFORE address space gets fragmented by other allocations.
    // Without this, later Python calls to printf/write/malloc/etc trigger
    // mmap for CFI shadow entries, which fails on fragmented address space.
    {
        // stdio functions used by Python internals
        char buf[64];
        snprintf(buf, sizeof(buf), "warmup %d", 42);
        fprintf(stderr, "%s", "");  // Zero-length write exercises fprintf
        fflush(stderr);
        fflush(stdout);

        // Memory allocation (Python's allocator uses these)
        void* p = malloc(1024);
        if (p) {
            memset(p, 0, 1024);
            free(p);
        }
        p = calloc(1, 256);
        if (p) free(p);
        p = realloc(NULL, 128);
        if (p) free(p);

        // String operations (used throughout Python)
        char tmp[32];
        strncpy(tmp, "test", sizeof(tmp));
        strlen(tmp);
        strcmp(tmp, "test");
        memcpy(buf, tmp, 4);
        memmove(buf + 4, tmp, 4);
    }

    // Acquire GIL using our pre-created thread state
    int gil_handle = python_c_gil_acquire();

    // === Exercise all Python C API functions used in execution path ===

    // 1. Module and globals access (used in every eval/execute)
    PyObject* main_module = PyImport_AddModule("__main__");
    PyObject* globals = PyModule_GetDict(main_module);

    // 2. Expression evaluation (Py_eval_input) - the main execution path
    PyObject* result = PyRun_String("1+1", Py_eval_input, globals, globals);
    if (result) {
        // 3. Integer type checking and conversion
        if (PyLong_Check(result)) {
            long long ll_val = PyLong_AsLongLong(result);
            (void)ll_val;  // Suppress unused warning
        }

        // 4. Object-to-string conversion
        PyObject* str_repr = PyObject_Str(result);
        if (str_repr) {
            PyUnicode_AsUTF8(str_repr);
            Py_DECREF(str_repr);
        }

        Py_DECREF(result);
    } else {
        PyErr_Clear();
    }

    // 5. Statement execution (Py_file_input)
    result = PyRun_String("_warmup_var = 42", Py_file_input, globals, globals);
    Py_XDECREF(result);

    // 6. Float operations
    PyObject* float_val = PyFloat_FromDouble(3.14);
    if (float_val) {
        PyFloat_Check(float_val);
        PyFloat_AsDouble(float_val);
        Py_DECREF(float_val);
    }

    // 7. String operations
    PyObject* str_val = PyUnicode_FromString("warmup");
    if (str_val) {
        PyUnicode_Check(str_val);
        PyUnicode_AsUTF8(str_val);
        Py_DECREF(str_val);
    }

    // 8. Boolean operations
    PyBool_Check(Py_True);
    PyBool_Check(Py_False);

    // 9. None check
    (void)(Py_None);

    // 10. List operations (used for array conversion)
    PyObject* list = PyList_New(2);
    if (list) {
        PyList_SetItem(list, 0, PyLong_FromLong(1));   // Steals ref
        PyList_SetItem(list, 1, PyLong_FromLong(2));   // Steals ref
        PyList_Size(list);
        PyList_GetItem(list, 0);  // Borrowed ref
        PyList_Check(list);
        Py_DECREF(list);
    }

    // 11. Tuple operations (used for tuple->array conversion)
    PyObject* tuple = PyTuple_New(1);
    if (tuple) {
        PyTuple_SetItem(tuple, 0, PyLong_FromLong(42));  // Steals ref
        PyTuple_Size(tuple);
        PyTuple_GetItem(tuple, 0);  // Borrowed ref
        PyTuple_Check(tuple);
        Py_DECREF(tuple);
    }

    // 12. Dict operations (used for dict conversion)
    PyObject* dict = PyDict_New();
    if (dict) {
        PyObject* key = PyUnicode_FromString("key");
        PyObject* val = PyLong_FromLong(99);
        PyDict_SetItem(dict, key, val);
        Py_DECREF(key);
        Py_DECREF(val);

        // Dict iteration
        PyObject *dk, *dv;
        Py_ssize_t pos = 0;
        while (PyDict_Next(dict, &pos, &dk, &dv)) {
            // Just iterate - exercises the function
        }
        PyDict_Check(dict);
        Py_DECREF(dict);
    }

    // 13. Error handling functions
    PyErr_Clear();
    // Trigger and fetch an error
    PyObject* bad_result = PyRun_String("undefined_var_xyz", Py_eval_input, globals, globals);
    if (!bad_result) {
        PyObject *ptype, *pvalue, *ptraceback;
        PyErr_Fetch(&ptype, &pvalue, &ptraceback);
        if (pvalue) {
            PyObject* err_str = PyObject_Str(pvalue);
            if (err_str) Py_DECREF(err_str);
        }
        Py_XDECREF(ptype);
        Py_XDECREF(pvalue);
        Py_XDECREF(ptraceback);
    } else {
        Py_DECREF(bad_result);
    }

    // 14. Long conversion edge cases
    PyObject* big_int = PyLong_FromLongLong(2147483647LL);
    if (big_int) {
        PyLong_AsDouble(big_int);
        Py_DECREF(big_int);
    }

    // 15. Py_INCREF/Py_DECREF/Py_XDECREF exercise
    Py_INCREF(Py_None);
    Py_DECREF(Py_None);

    // Clean up warmup variable
    result = PyRun_String("del _warmup_var", Py_file_input, globals, globals);
    Py_XDECREF(result);

    // Release GIL
    python_c_gil_release(gil_handle);

}

/**
 * Shutdown Python
 */
int python_c_shutdown(void) {
    if (!python_initialized) {
        fprintf(stderr, "[PythonC] Not initialized\n");
        return -1;
    }

    // Skip Py_Finalize on Android.
    // Py_Finalize triggers bionic CFI crashes when thread pool workers
    // have touched Python (CFI shadow memory mmap fails during finalization).
    // This is safe: the OS cleans up all process resources on exit.
    // Many Python embedding applications skip Py_Finalize for similar reasons.
    python_initialized = 0;
    main_thread_state = NULL;

    return 0;
}
