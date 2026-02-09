// Phase 1 Item 10 Day 5: Polyglot Async Integration
// Implementation of thread-safe async execution for polyglot blocks

#include "naab/polyglot_async_executor.h"
#include "naab/python_executor.h"
#include "naab/python_interpreter_manager.h"  // Global Python interpreter manager
#include "naab/js_executor.h"
#include "naab/cpp_executor_adapter.h"  // Expression-oriented inline C++ executor
#include "naab/rust_executor.h"
#include "naab/csharp_executor.h"
#include "naab/shell_executor.h"
#include "naab/generic_subprocess_executor.h"
#include "naab/audit_logger.h"
#include <fmt/format.h>
#include <mutex>
#include <memory>
#include <future>

namespace naab {
namespace polyglot {

// ============================================================================
// Python Async Executor Implementation
// ============================================================================

// Python executor - using global interpreter manager
// Interpreter initialized once globally, each thread creates its own executor with GIL

std::future<ffi::AsyncCallbackResult> PythonAsyncExecutor::executeAsync(
    const std::string& code,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    // Ensure global Python interpreter is initialized once
    if (!::naab::runtime::PythonInterpreterManager::isInitialized()) {
        ::naab::runtime::PythonInterpreterManager::initialize();
    }

    auto callback = makePythonCallback(code, args);

    // Keep wrapper alive on heap
    auto wrapper = std::make_shared<ffi::AsyncCallbackWrapper>(
        std::move(callback),
        "python_async",
        timeout
    );

    auto future = wrapper->executeAsync();

    // Wrap to extend wrapper's lifetime - use async launch to actually run in parallel
    return std::async(std::launch::async, [wrapper, future = std::move(future)]() mutable {
        return future.get();
    });
}

ffi::AsyncCallbackResult PythonAsyncExecutor::executeBlocking(
    const std::string& code,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    // Ensure global Python interpreter is initialized
    if (!::naab::runtime::PythonInterpreterManager::isInitialized()) {
        ::naab::runtime::PythonInterpreterManager::initialize();
    }

    auto callback = makePythonCallback(code, args);

    ffi::AsyncCallbackWrapper wrapper(
        std::move(callback),
        "python_blocking",
        timeout
    );

    return wrapper.executeBlocking();
}

ffi::AsyncCallbackWrapper::CallbackFunc PythonAsyncExecutor::makePythonCallback(
    const std::string& code,
    const std::vector<interpreter::Value>& args
) {
    // Capture code and args by value for thread safety
    return [code, args]() -> interpreter::Value {
        // Python callback invoked (silent)

        security::AuditLogger::log(
            security::AuditEvent::BLOCK_EXECUTE,
            fmt::format("Executing Python code asynchronously ({} bytes)", code.size())
        );

        try {
            // About to acquire GIL (silent)

            interpreter::Value result_value;

            {
                // Acquire GIL for this thread (required for multi-threaded Python access)
                py::gil_scoped_acquire gil;

                // GIL acquired successfully (silent)

                // Create a fresh Python executor for this thread
                // Global interpreter is already initialized, executor just accesses globals
                // Skip stdout/stderr redirection for async execution to avoid conflicts
                // Creating PythonExecutor (silent)
                runtime::PythonExecutor executor(false);

                // PythonExecutor created (silent)

                // Execute Python code (GIL already acquired)
                auto result_ptr = executor.executeWithResult(code);

                // executeWithResult returned (silent)

                if (!result_ptr) {
                    throw std::runtime_error("Python execution returned null result");
                }

                // Dereference shared_ptr to get Value
                // Dereferencing result_ptr (silent)
                result_value = *result_ptr;

                // Explicitly release the shared_ptr while GIL is still held
                // Releasing result_ptr (silent)
                result_ptr.reset();

                // About to exit GIL scope (silent)
                // executor and gil are destroyed here
            }

            // GIL released (silent)
            return result_value;

        } catch (const std::exception& e) {
            // Exception caught (silent - will be logged by audit logger)
            security::AuditLogger::logSecurityViolation(
                fmt::format("python_async_exception: {}", e.what())
            );
            throw;  // Re-throw for async callback handler
        }

        // Lambda about to exit (silent)
    };
}

// ============================================================================
// JavaScript Async Executor Implementation
// ============================================================================

std::future<ffi::AsyncCallbackResult> JavaScriptAsyncExecutor::executeAsync(
    const std::string& code,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    auto callback = makeJavaScriptCallback(code, args);

    // Keep wrapper alive on heap
    auto wrapper = std::make_shared<ffi::AsyncCallbackWrapper>(
        std::move(callback),
        "javascript_async",
        timeout
    );

    auto future = wrapper->executeAsync();

    // Wrap to extend wrapper's lifetime - use async launch to actually run in parallel
    return std::async(std::launch::async, [wrapper, future = std::move(future)]() mutable {
        return future.get();
    });
}

ffi::AsyncCallbackResult JavaScriptAsyncExecutor::executeBlocking(
    const std::string& code,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    auto callback = makeJavaScriptCallback(code, args);

    ffi::AsyncCallbackWrapper wrapper(
        std::move(callback),
        "javascript_blocking",
        timeout
    );

    return wrapper.executeBlocking();
}

ffi::AsyncCallbackWrapper::CallbackFunc JavaScriptAsyncExecutor::makeJavaScriptCallback(
    const std::string& code,
    const std::vector<interpreter::Value>& args
) {
    // Capture code and args by value
    return [code, args]() -> interpreter::Value {
        security::AuditLogger::log(
            security::AuditEvent::BLOCK_EXECUTE,
            fmt::format("Executing JavaScript code asynchronously ({} bytes)", code.size())
        );

        try {
            // Create fresh executor (no state to preserve between calls)
            auto executor = std::make_unique<runtime::JsExecutor>();

            if (!executor->isInitialized()) {
                throw std::runtime_error("JavaScript executor failed to initialize");
            }

            // Execute JavaScript code
            auto result_ptr = executor->evaluate(code);

            if (!result_ptr) {
                throw std::runtime_error("JavaScript execution returned null result");
            }

            // Dereference shared_ptr to get Value
            return *result_ptr;

        } catch (const std::exception& e) {
            security::AuditLogger::logSecurityViolation(
                fmt::format("javascript_async_exception: {}", e.what())
            );
            throw;
        }
    };
}

// ============================================================================
// C++ Async Executor Implementation
// ============================================================================

std::future<ffi::AsyncCallbackResult> CppAsyncExecutor::executeAsync(
    const std::string& code,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    auto callback = makeCppCallback(code, args);

    // Keep wrapper alive on heap
    auto wrapper = std::make_shared<ffi::AsyncCallbackWrapper>(
        std::move(callback),
        "cpp_async",
        timeout
    );

    auto future = wrapper->executeAsync();

    // Wrap to extend wrapper's lifetime - use async launch to actually run in parallel
    return std::async(std::launch::async, [wrapper, future = std::move(future)]() mutable {
        return future.get();
    });
}

ffi::AsyncCallbackResult CppAsyncExecutor::executeBlocking(
    const std::string& code,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    auto callback = makeCppCallback(code, args);

    ffi::AsyncCallbackWrapper wrapper(
        std::move(callback),
        "cpp_blocking",
        timeout
    );

    return wrapper.executeBlocking();
}

ffi::AsyncCallbackWrapper::CallbackFunc CppAsyncExecutor::makeCppCallback(
    const std::string& code,
    const std::vector<interpreter::Value>& args
) {
    // Capture code by value for thread safety (like Python and JavaScript)
    return [code, args]() -> interpreter::Value {
        security::AuditLogger::log(
            security::AuditEvent::BLOCK_EXECUTE,
            fmt::format("Executing C++ code asynchronously ({} bytes)", code.size())
        );

        try {
            // Create a fresh C++ executor adapter for this thread
            // (expression-oriented inline code execution with variable binding support)
            runtime::CppExecutorAdapter executor;

            // Execute C++ code with return value
            auto result_ptr = executor.executeWithReturn(code);

            if (!result_ptr) {
                throw std::runtime_error("C++ execution returned null result");
            }

            // Dereference shared_ptr to get Value
            return *result_ptr;

        } catch (const std::exception& e) {
            security::AuditLogger::logSecurityViolation(
                fmt::format("cpp_async_exception: {}", e.what())
            );
            throw;
        }
    };
}

// ============================================================================
// Rust Async Executor Implementation
// ============================================================================

RustAsyncExecutor::RustAsyncExecutor() = default;
RustAsyncExecutor::~RustAsyncExecutor() = default;

std::future<ffi::AsyncCallbackResult> RustAsyncExecutor::executeAsync(
    const std::string& code,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    auto callback = makeRustCallback(code, args);

    // Keep wrapper alive on heap
    auto wrapper = std::make_shared<ffi::AsyncCallbackWrapper>(
        std::move(callback),
        "rust_async",
        timeout
    );

    auto future = wrapper->executeAsync();

    // Wrap to extend wrapper's lifetime - use async launch to actually run in parallel
    return std::async(std::launch::async, [wrapper, future = std::move(future)]() mutable {
        return future.get();
    });
}

ffi::AsyncCallbackResult RustAsyncExecutor::executeBlocking(
    const std::string& code,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    auto callback = makeRustCallback(code, args);

    ffi::AsyncCallbackWrapper wrapper(
        std::move(callback),
        "rust_blocking",
        timeout
    );

    return wrapper.executeBlocking();
}

ffi::AsyncCallbackWrapper::CallbackFunc RustAsyncExecutor::makeRustCallback(
    const std::string& code,
    const std::vector<interpreter::Value>& args
) {
    // Capture code and args by value (no 'this' to avoid dangling pointer)
    return [code, args]() -> interpreter::Value {
        security::AuditLogger::log(
            security::AuditEvent::BLOCK_EXECUTE,
            fmt::format("Executing Rust code asynchronously ({} bytes)", code.size())
        );

        try {
            // Create fresh executor (subprocess-based, no state to preserve)
            auto executor = std::make_unique<runtime::RustExecutor>();

            // CRITICAL FIX: Use executeWithReturn for inline code (not executeBlock for FFI)
            auto result_ptr = executor->executeWithReturn(code);

            if (!result_ptr) {
                throw std::runtime_error("Rust execution returned null result");
            }

            return *result_ptr;

        } catch (const std::exception& e) {
            security::AuditLogger::logSecurityViolation(
                fmt::format("rust_async_exception: {}", e.what())
            );
            throw;
        }
    };
}

// ============================================================================
// C# Async Executor Implementation
// ============================================================================

std::future<ffi::AsyncCallbackResult> CSharpAsyncExecutor::executeAsync(
    const std::string& code,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    auto callback = makeCSharpCallback(code, args);

    // Keep wrapper alive on heap
    auto wrapper = std::make_shared<ffi::AsyncCallbackWrapper>(
        std::move(callback),
        "csharp_async",
        timeout
    );

    auto future = wrapper->executeAsync();

    // Wrap to extend wrapper's lifetime - use async launch to actually run in parallel
    return std::async(std::launch::async, [wrapper, future = std::move(future)]() mutable {
        return future.get();
    });
}

ffi::AsyncCallbackResult CSharpAsyncExecutor::executeBlocking(
    const std::string& code,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    auto callback = makeCSharpCallback(code, args);

    ffi::AsyncCallbackWrapper wrapper(
        std::move(callback),
        "csharp_blocking",
        timeout
    );

    return wrapper.executeBlocking();
}

ffi::AsyncCallbackWrapper::CallbackFunc CSharpAsyncExecutor::makeCSharpCallback(
    const std::string& code,
    const std::vector<interpreter::Value>& args
) {
    // Capture code and args by value (no 'this' capture to avoid dangling pointer)
    return [code, args]() -> interpreter::Value {
        security::AuditLogger::log(
            security::AuditEvent::BLOCK_EXECUTE,
            fmt::format("Executing C# code asynchronously ({} bytes)", code.size())
        );

        try {
            // Create fresh executor (subprocess-based, no state to preserve)
            auto executor = std::make_unique<runtime::CSharpExecutor>();

            // Execute C# code
            auto result_ptr = executor->executeWithReturn(code);

            if (!result_ptr) {
                throw std::runtime_error("C# execution returned null result");
            }

            return *result_ptr;

        } catch (const std::exception& e) {
            security::AuditLogger::logSecurityViolation(
                fmt::format("csharp_async_exception: {}", e.what())
            );
            throw;
        }
    };
}

// ============================================================================
// Shell Async Executor Implementation
// ============================================================================

std::future<ffi::AsyncCallbackResult> ShellAsyncExecutor::executeAsync(
    const std::string& command,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    auto callback = makeShellCallback(command, args);

    // Keep wrapper alive on heap - it will be cleaned up when future completes
    auto wrapper = std::make_shared<ffi::AsyncCallbackWrapper>(
        std::move(callback),
        "shell_async",
        timeout
    );

    // Capture wrapper in a lambda to keep it alive until future completes
    auto future = wrapper->executeAsync();

    // Wrap the future to extend wrapper's lifetime
    return std::async(std::launch::deferred, [wrapper, future = std::move(future)]() mutable {
        return future.get();
    });
}

ffi::AsyncCallbackResult ShellAsyncExecutor::executeBlocking(
    const std::string& command,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    auto callback = makeShellCallback(command, args);

    ffi::AsyncCallbackWrapper wrapper(
        std::move(callback),
        "shell_blocking",
        timeout
    );

    return wrapper.executeBlocking();
}

ffi::AsyncCallbackWrapper::CallbackFunc ShellAsyncExecutor::makeShellCallback(
    const std::string& command,
    const std::vector<interpreter::Value>& args
) {
    // Capture command and args by value
    return [command, args]() -> interpreter::Value {
        security::AuditLogger::log(
            security::AuditEvent::BLOCK_EXECUTE,
            fmt::format("Executing shell command asynchronously: {}", command)
        );

        try {
            // Create fresh executor (no state to preserve between calls)
            auto executor = std::make_unique<runtime::ShellExecutor>();

            // Execute shell command
            auto result_ptr = executor->executeWithReturn(command);

            if (!result_ptr) {
                throw std::runtime_error("Shell execution returned null result");
            }

            return *result_ptr;

        } catch (const std::exception& e) {
            security::AuditLogger::logSecurityViolation(
                fmt::format("shell_async_exception: {}", e.what())
            );
            throw;
        }
    };
}

// ============================================================================
// Generic Subprocess Async Executor Implementation
// ============================================================================

GenericSubprocessAsyncExecutor::GenericSubprocessAsyncExecutor(
    const std::string& language_id,
    const std::string& command_template,
    const std::string& file_extension
)
    : language_id_(language_id)
    , command_template_(command_template)
    , file_extension_(file_extension)
{
    // Configuration stored in members, executor created fresh in each callback
}

GenericSubprocessAsyncExecutor::~GenericSubprocessAsyncExecutor() = default;

std::future<ffi::AsyncCallbackResult> GenericSubprocessAsyncExecutor::executeAsync(
    const std::string& code,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    auto callback = makeSubprocessCallback(code, args);

    // Keep wrapper alive on heap
    auto wrapper = std::make_shared<ffi::AsyncCallbackWrapper>(
        std::move(callback),
        fmt::format("{}_async", language_id_),
        timeout
    );

    auto future = wrapper->executeAsync();

    // Wrap to extend wrapper's lifetime - use async launch to actually run in parallel
    return std::async(std::launch::async, [wrapper, future = std::move(future)]() mutable {
        return future.get();
    });
}

ffi::AsyncCallbackResult GenericSubprocessAsyncExecutor::executeBlocking(
    const std::string& code,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    auto callback = makeSubprocessCallback(code, args);

    ffi::AsyncCallbackWrapper wrapper(
        std::move(callback),
        fmt::format("{}_blocking", language_id_),
        timeout
    );

    return wrapper.executeBlocking();
}

ffi::AsyncCallbackWrapper::CallbackFunc GenericSubprocessAsyncExecutor::makeSubprocessCallback(
    const std::string& code,
    const std::vector<interpreter::Value>& args
) {
    // Capture configuration and code by value (no 'this' to avoid dangling pointer)
    std::string lang_id = language_id_;
    std::string cmd_template = command_template_;
    std::string file_ext = file_extension_;

    return [lang_id, cmd_template, file_ext, code, args]() -> interpreter::Value {
        security::AuditLogger::log(
            security::AuditEvent::BLOCK_EXECUTE,
            fmt::format("Executing {} code asynchronously ({} bytes)",
                       lang_id, code.size())
        );

        try {
            // Create fresh executor with configuration
            auto executor = std::make_unique<runtime::GenericSubprocessExecutor>(
                lang_id, cmd_template, file_ext
            );

            // Execute via subprocess
            auto result_ptr = executor->executeWithReturn(code);

            if (!result_ptr) {
                throw std::runtime_error(
                    fmt::format("{} execution returned null result", lang_id)
                );
            }

            return *result_ptr;

        } catch (const std::exception& e) {
            security::AuditLogger::logSecurityViolation(
                fmt::format("{}_async_exception: {}", lang_id, e.what())
            );
            throw;
        }
    };
}

// ============================================================================
// Unified Polyglot Async Executor Implementation
// ============================================================================

std::future<ffi::AsyncCallbackResult> PolyglotAsyncExecutor::executeAsync(
    Language language,
    const std::string& code,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    security::AuditLogger::log(
        security::AuditEvent::BLOCK_EXECUTE,
        fmt::format("Polyglot async execution: {} ({} bytes)",
                   languageToString(language), code.size())
    );

    switch (language) {
        case Language::Python:
            return python_executor_.executeAsync(code, args, timeout);

        case Language::JavaScript:
            return js_executor_.executeAsync(code, args, timeout);

        case Language::Cpp:
            return cpp_executor_.executeAsync(code, args, timeout);

        case Language::Rust:
            return rust_executor_.executeAsync(code, args, timeout);

        case Language::CSharp:
            return csharp_executor_.executeAsync(code, args, timeout);

        case Language::Shell:
            return shell_executor_.executeAsync(code, args, timeout);

        case Language::GenericSubprocess:
            throw std::runtime_error(
                "GenericSubprocess requires explicit executor with command template"
            );

        default:
            throw std::runtime_error("Unknown polyglot language");
    }
}

ffi::AsyncCallbackResult PolyglotAsyncExecutor::executeBlocking(
    Language language,
    const std::string& code,
    const std::vector<interpreter::Value>& args,
    std::chrono::milliseconds timeout
) {
    switch (language) {
        case Language::Python:
            return python_executor_.executeBlocking(code, args, timeout);

        case Language::JavaScript:
            return js_executor_.executeBlocking(code, args, timeout);

        case Language::Cpp:
            return cpp_executor_.executeBlocking(code, args, timeout);

        case Language::Rust:
            return rust_executor_.executeBlocking(code, args, timeout);

        case Language::CSharp:
            return csharp_executor_.executeBlocking(code, args, timeout);

        case Language::Shell:
            return shell_executor_.executeBlocking(code, args, timeout);

        case Language::GenericSubprocess:
            throw std::runtime_error(
                "GenericSubprocess requires explicit executor with command template"
            );

        default:
            throw std::runtime_error("Unknown polyglot language");
    }
}

std::vector<ffi::AsyncCallbackResult> PolyglotAsyncExecutor::executeParallel(
    const std::vector<std::tuple<Language, std::string, std::vector<interpreter::Value>>>& blocks,
    std::chrono::milliseconds timeout
) {
    security::AuditLogger::log(
        security::AuditEvent::BLOCK_EXECUTE,
        fmt::format("Executing {} polyglot blocks in parallel", blocks.size())
    );

    // Launch all blocks asynchronously
    std::vector<std::future<ffi::AsyncCallbackResult>> futures;
    futures.reserve(blocks.size());

    for (const auto& [language, code, args] : blocks) {
        futures.push_back(executeAsync(language, code, args, timeout));
    }

    // Collect results
    std::vector<ffi::AsyncCallbackResult> results;
    results.reserve(futures.size());

    for (auto& future : futures) {
        results.push_back(future.get());
    }

    security::AuditLogger::log(
        security::AuditEvent::BLOCK_EXECUTE,
        fmt::format("Parallel polyglot execution completed ({} blocks)", results.size())
    );

    return results;
}

std::string PolyglotAsyncExecutor::languageToString(Language lang) {
    switch (lang) {
        case Language::Python: return "Python";
        case Language::JavaScript: return "JavaScript";
        case Language::Cpp: return "C++";
        case Language::Rust: return "Rust";
        case Language::CSharp: return "C#";
        case Language::Shell: return "Shell";
        case Language::GenericSubprocess: return "GenericSubprocess";
        default: return "Unknown";
    }
}

} // namespace polyglot
} // namespace naab
