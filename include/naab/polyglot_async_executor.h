// Phase 1 Item 10 Day 5: Polyglot Async Integration
// Thread-safe async execution for Python/JavaScript/C++ polyglot blocks

#ifndef NAAB_POLYGLOT_ASYNC_EXECUTOR_H
#define NAAB_POLYGLOT_ASYNC_EXECUTOR_H

#include "naab/ffi_async_callback.h"
#include "naab/value.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace naab {

// Forward declarations for runtime executors
namespace runtime {
    class PythonExecutor;
    class JsExecutor;
    class CppExecutor;
    class RustExecutor;
    class CSharpExecutor;
    class ShellExecutor;
    class GenericSubprocessExecutor;
}

namespace polyglot {

// ============================================================================
// Python Async Executor
// ============================================================================

/**
 * @brief Executes Python code asynchronously with proper GIL handling.
 *
 * Python has a Global Interpreter Lock (GIL) that must be acquired before
 * calling any Python API. This class ensures thread-safe async execution
 * of Python code by properly managing the GIL.
 */
class PythonAsyncExecutor {
public:
    /**
     * @brief Execute Python code asynchronously.
     *
     * @param code Python code to execute
     * @param args Arguments to pass to Python
     * @param timeout Maximum execution time (default: 30s)
     * @return Future that resolves to the execution result
     *
     * Example:
     *   PythonAsyncExecutor executor;
     *   auto future = executor.executeAsync("return 2 + 2", {});
     *   auto result = future.get();  // AsyncCallbackResult
     */
    std::future<ffi::AsyncCallbackResult> executeAsync(
        const std::string& code,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

    /**
     * @brief Execute Python code synchronously (blocking).
     *
     * @param code Python code to execute
     * @param args Arguments to pass to Python
     * @param timeout Maximum execution time
     * @return Execution result
     */
    ffi::AsyncCallbackResult executeBlocking(
        const std::string& code,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

private:
    // Note: Python executor is a singleton (managed internally in .cpp file)
    // Python's interpreter can only be initialized once per process

    // Helper: Create callback that executes Python code
    ffi::AsyncCallbackWrapper::CallbackFunc makePythonCallback(
        const std::string& code,
        const std::vector<interpreter::Value>& args
    );
};

// ============================================================================
// JavaScript Async Executor
// ============================================================================

/**
 * @brief Executes JavaScript code asynchronously.
 *
 * JavaScript engines are typically single-threaded, so this executor
 * creates isolated JS contexts per thread for safe concurrent execution.
 */
class JavaScriptAsyncExecutor {
public:
    /**
     * @brief Execute JavaScript code asynchronously.
     *
     * @param code JavaScript code to execute
     * @param args Arguments to pass to JavaScript
     * @param timeout Maximum execution time (default: 30s)
     * @return Future that resolves to the execution result
     */
    std::future<ffi::AsyncCallbackResult> executeAsync(
        const std::string& code,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

    /**
     * @brief Execute JavaScript code synchronously (blocking).
     */
    ffi::AsyncCallbackResult executeBlocking(
        const std::string& code,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

private:
    // Helper: Create callback that executes JavaScript code
    ffi::AsyncCallbackWrapper::CallbackFunc makeJavaScriptCallback(
        const std::string& code,
        const std::vector<interpreter::Value>& args
    );
};

// ============================================================================
// C++ Async Executor
// ============================================================================

/**
 * @brief Executes C++ code asynchronously.
 *
 * C++ polyglot blocks are compiled and executed. This executor manages
 * compilation and execution in separate threads with timeout protection.
 */
class CppAsyncExecutor {
public:
    /**
     * @brief Execute C++ code asynchronously.
     *
     * @param code C++ code to compile and execute
     * @param args Arguments to pass to C++
     * @param timeout Maximum execution time (default: 60s for compilation)
     * @return Future that resolves to the execution result
     */
    std::future<ffi::AsyncCallbackResult> executeAsync(
        const std::string& code,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(60000)
    );

    /**
     * @brief Execute C++ code synchronously (blocking).
     */
    ffi::AsyncCallbackResult executeBlocking(
        const std::string& code,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(60000)
    );

private:
    // C++ executor instance
    std::unique_ptr<runtime::CppExecutor> executor_;

    // Helper: Create callback that executes C++ code
    ffi::AsyncCallbackWrapper::CallbackFunc makeCppCallback(
        const std::string& code,
        const std::vector<interpreter::Value>& args
    );
};

// ============================================================================
// Rust Async Executor
// ============================================================================

/**
 * @brief Executes Rust code asynchronously.
 *
 * Rust polyglot blocks use pre-compiled .so libraries loaded via FFI.
 * Format: "rust://path/to/lib.so::function_name"
 */
class RustAsyncExecutor {
public:
    RustAsyncExecutor();
    ~RustAsyncExecutor();

    /**
     * @brief Execute Rust code asynchronously.
     *
     * @param uri Rust block URI (e.g., "rust://./lib.so::process")
     * @param args Arguments to pass to Rust function
     * @param timeout Maximum execution time (default: 30s)
     * @return Future that resolves to the execution result
     */
    std::future<ffi::AsyncCallbackResult> executeAsync(
        const std::string& uri,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

    /**
     * @brief Execute Rust code synchronously (blocking).
     */
    ffi::AsyncCallbackResult executeBlocking(
        const std::string& uri,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

private:
    // Rust executor instance
    std::unique_ptr<runtime::RustExecutor> executor_;

    // Helper: Create callback that executes Rust code
    ffi::AsyncCallbackWrapper::CallbackFunc makeRustCallback(
        const std::string& uri,
        const std::vector<interpreter::Value>& args
    );
};

// ============================================================================
// C# Async Executor
// ============================================================================

/**
 * @brief Executes C# code asynchronously.
 *
 * C# code is executed via Mono runtime or .NET Core.
 */
class CSharpAsyncExecutor {
public:
    /**
     * @brief Execute C# code asynchronously.
     */
    std::future<ffi::AsyncCallbackResult> executeAsync(
        const std::string& code,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

    /**
     * @brief Execute C# code synchronously (blocking).
     */
    ffi::AsyncCallbackResult executeBlocking(
        const std::string& code,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

private:
    // Helper: Create callback that executes C# code
    ffi::AsyncCallbackWrapper::CallbackFunc makeCSharpCallback(
        const std::string& code,
        const std::vector<interpreter::Value>& args
    );
};

// ============================================================================
// Shell Async Executor
// ============================================================================

/**
 * @brief Executes shell commands asynchronously.
 *
 * Shell commands are executed via subprocess with timeout protection.
 */
class ShellAsyncExecutor {
public:
    /**
     * @brief Execute shell command asynchronously.
     */
    std::future<ffi::AsyncCallbackResult> executeAsync(
        const std::string& command,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

    /**
     * @brief Execute shell command synchronously (blocking).
     */
    ffi::AsyncCallbackResult executeBlocking(
        const std::string& command,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

private:
    // Helper: Create callback that executes shell command
    ffi::AsyncCallbackWrapper::CallbackFunc makeShellCallback(
        const std::string& command,
        const std::vector<interpreter::Value>& args
    );
};

// ============================================================================
// Generic Subprocess Async Executor
// ============================================================================

/**
 * @brief Executes code via generic subprocess (Ruby, Go, Perl, etc.).
 *
 * Supports any language that can be executed via command-line tool.
 */
class GenericSubprocessAsyncExecutor {
public:
    /**
     * @brief Constructor.
     *
     * @param language_id Language identifier (e.g., "ruby", "go")
     * @param command_template Command template (e.g., "ruby -e {}", "go run {}")
     * @param file_extension File extension if needed (e.g., ".rb", ".go")
     */
    GenericSubprocessAsyncExecutor(
        const std::string& language_id,
        const std::string& command_template,
        const std::string& file_extension = ""
    );
    ~GenericSubprocessAsyncExecutor();

    /**
     * @brief Execute code asynchronously.
     */
    std::future<ffi::AsyncCallbackResult> executeAsync(
        const std::string& code,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

    /**
     * @brief Execute code synchronously (blocking).
     */
    ffi::AsyncCallbackResult executeBlocking(
        const std::string& code,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

private:
    std::string language_id_;
    std::string command_template_;
    std::string file_extension_;

    // Helper: Create callback that executes code via subprocess
    ffi::AsyncCallbackWrapper::CallbackFunc makeSubprocessCallback(
        const std::string& code,
        const std::vector<interpreter::Value>& args
    );
};

// ============================================================================
// Unified Polyglot Async Executor
// ============================================================================

/**
 * @brief Unified async executor for all polyglot languages.
 *
 * Automatically routes to the correct executor based on language.
 */
class PolyglotAsyncExecutor {
public:
    enum class Language {
        Python,
        JavaScript,
        Cpp,
        Rust,
        CSharp,
        Shell,
        GenericSubprocess
    };

    /**
     * @brief Execute polyglot code asynchronously.
     *
     * @param language Target language
     * @param code Code to execute
     * @param args Arguments
     * @param timeout Maximum execution time
     * @return Future that resolves to the execution result
     */
    std::future<ffi::AsyncCallbackResult> executeAsync(
        Language language,
        const std::string& code,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

    /**
     * @brief Execute polyglot code synchronously.
     */
    ffi::AsyncCallbackResult executeBlocking(
        Language language,
        const std::string& code,
        const std::vector<interpreter::Value>& args,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

    /**
     * @brief Execute multiple polyglot blocks in parallel.
     *
     * @param blocks Vector of (language, code, args) tuples
     * @param timeout Timeout for each block
     * @return Vector of results (same order as input)
     */
    std::vector<ffi::AsyncCallbackResult> executeParallel(
        const std::vector<std::tuple<Language, std::string, std::vector<interpreter::Value>>>& blocks,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
    );

private:
    PythonAsyncExecutor python_executor_;
    JavaScriptAsyncExecutor js_executor_;
    CppAsyncExecutor cpp_executor_;
    RustAsyncExecutor rust_executor_;
    CSharpAsyncExecutor csharp_executor_;
    ShellAsyncExecutor shell_executor_;
    // Note: GenericSubprocessAsyncExecutor requires constructor args,
    // so it's created on-demand rather than stored as a member

    // Helper: Convert language enum to string
    static std::string languageToString(Language lang);
};

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Execute Python code asynchronously (convenience function).
 */
inline std::future<ffi::AsyncCallbackResult> executePythonAsync(
    const std::string& code,
    const std::vector<interpreter::Value>& args = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
) {
    PythonAsyncExecutor executor;
    return executor.executeAsync(code, args, timeout);
}

/**
 * @brief Execute JavaScript code asynchronously (convenience function).
 */
inline std::future<ffi::AsyncCallbackResult> executeJavaScriptAsync(
    const std::string& code,
    const std::vector<interpreter::Value>& args = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
) {
    JavaScriptAsyncExecutor executor;
    return executor.executeAsync(code, args, timeout);
}

/**
 * @brief Execute C++ code asynchronously (convenience function).
 */
inline std::future<ffi::AsyncCallbackResult> executeCppAsync(
    const std::string& code,
    const std::vector<interpreter::Value>& args = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds(60000)
) {
    CppAsyncExecutor executor;
    return executor.executeAsync(code, args, timeout);
}

/**
 * @brief Execute Rust code asynchronously (convenience function).
 */
inline std::future<ffi::AsyncCallbackResult> executeRustAsync(
    const std::string& uri,
    const std::vector<interpreter::Value>& args = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
) {
    RustAsyncExecutor executor;
    return executor.executeAsync(uri, args, timeout);
}

/**
 * @brief Execute C# code asynchronously (convenience function).
 */
inline std::future<ffi::AsyncCallbackResult> executeCSharpAsync(
    const std::string& code,
    const std::vector<interpreter::Value>& args = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
) {
    CSharpAsyncExecutor executor;
    return executor.executeAsync(code, args, timeout);
}

/**
 * @brief Execute shell command asynchronously (convenience function).
 */
inline std::future<ffi::AsyncCallbackResult> executeShellAsync(
    const std::string& command,
    const std::vector<interpreter::Value>& args = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds(30000)
) {
    ShellAsyncExecutor executor;
    return executor.executeAsync(command, args, timeout);
}

} // namespace polyglot
} // namespace naab

#endif // NAAB_POLYGLOT_ASYNC_EXECUTOR_H
