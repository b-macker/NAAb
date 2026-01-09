// NAAb Python Block Executor Implementation
// Embeds CPython interpreter using pybind11

#include "naab/python_executor.h"
#include "naab/interpreter.h"
#include "naab/resource_limits.h"
#include "naab/audit_logger.h"
#include "naab/sandbox.h"
#include <fmt/core.h>
#include <stdexcept>
#include <unordered_map>

namespace naab {
namespace runtime {

PythonExecutor::PythonExecutor()
    : guard_(), global_namespace_(py::globals()) {

    fmt::print("[Python] Initialized CPython interpreter\n");

    // Import commonly used modules
    try {
        py::module_::import("sys");
        py::module_::import("os");
        fmt::print("[Python] Imported standard modules (sys, os)\n");
    } catch (const py::error_already_set& e) {
        fmt::print("[WARN] Failed to import standard modules: {}\n", e.what());
    }
}

PythonExecutor::~PythonExecutor() {
    fmt::print("[Python] Shutting down CPython interpreter\n");
}

void PythonExecutor::execute(const std::string& code) {
    // Check sandbox permissions for code execution
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(security::Capability::BLOCK_CALL)) {
        fmt::print("[ERROR] Sandbox violation: Python execution denied\n");
        sandbox->logViolation("executePython", "<code>", "BLOCK_CALL capability required");
        throw std::runtime_error("Python execution denied by sandbox");
    }

    try {
        // Execute with 30-second timeout
        security::ScopedTimeout timeout(30);
        py::exec(code, global_namespace_);
        fmt::print("[Python] Executed code successfully\n");
    } catch (const security::ResourceLimitException& e) {
        fmt::print("[ERROR] Python execution timeout: {}\n", e.what());
        security::AuditLogger::logTimeout("Python exec()", 30);
        throw std::runtime_error("Python execution timed out");
    } catch (const py::error_already_set& e) {
        throw std::runtime_error(fmt::format("Python execution error: {}", e.what()));
    }
}

std::shared_ptr<interpreter::Value> PythonExecutor::executeWithResult(const std::string& code) {
    try {
        // Execute with 30-second timeout
        security::ScopedTimeout timeout(30);
        py::object result = py::eval(code, global_namespace_);
        return pythonToValue(result);
    } catch (const security::ResourceLimitException& e) {
        fmt::print("[ERROR] Python eval timeout: {}\n", e.what());
        security::AuditLogger::logTimeout("Python eval()", 30);
        throw std::runtime_error("Python eval timed out");
    } catch (const py::error_already_set& e) {
        throw std::runtime_error(fmt::format("Python eval error: {}", e.what()));
    }
}

std::shared_ptr<interpreter::Value> PythonExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    fmt::print("[Python] Calling function: {}\n", function_name);

    // Check if function exists
    if (!global_namespace_.contains(function_name.c_str())) {
        throw std::runtime_error(fmt::format("Python function not found: {}", function_name));
    }

    try {
        // Get the function object
        py::object func = global_namespace_[function_name.c_str()];

        // Convert NAAb arguments to Python objects
        py::list py_args;
        for (const auto& arg : args) {
            py_args.append(valueToPython(arg));
        }

        // Call the function with 30-second timeout
        py::object result;
        {
            security::ScopedTimeout timeout(30);
            result = func(*py_args);
        }

        // Convert result back to NAAb Value
        return pythonToValue(result);
    } catch (const security::ResourceLimitException& e) {
        fmt::print("[ERROR] Python function timeout: {}\n", e.what());
        security::AuditLogger::logTimeout("Python function: " + function_name, 30);
        throw std::runtime_error("Python function call timed out");
    } catch (const py::error_already_set& e) {
        throw std::runtime_error(fmt::format("Python call error in {}: {}", function_name, e.what()));
    }
}

bool PythonExecutor::loadModule(const std::string& module_name, const std::string& code) {
    fmt::print("[Python] Loading module: {}\n", module_name);

    try {
        // Execute the module code in the global namespace
        py::exec(code, global_namespace_);
        fmt::print("[Python] Module {} loaded successfully\n", module_name);
        return true;
    } catch (const py::error_already_set& e) {
        fmt::print("[ERROR] Failed to load module {}: {}\n", module_name, e.what());
        return false;
    }
}

bool PythonExecutor::hasFunction(const std::string& function_name) const {
    return global_namespace_.contains(function_name.c_str());
}

// ============================================================================
// Type Conversion: NAAb Value → Python Object
// ============================================================================

py::object PythonExecutor::valueToPython(const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([](auto&& arg) -> py::object {
        using T = std::decay_t<decltype(arg)>;

        if constexpr (std::is_same_v<T, std::monostate>) {
            // null/void → None
            return py::none();
        }
        else if constexpr (std::is_same_v<T, int>) {
            return py::int_(arg);
        }
        else if constexpr (std::is_same_v<T, double>) {
            return py::float_(arg);
        }
        else if constexpr (std::is_same_v<T, bool>) {
            return py::bool_(arg);
        }
        else if constexpr (std::is_same_v<T, std::string>) {
            return py::str(arg);
        }
        else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
            // Array → Python list
            py::list result;
            for (const auto& item : arg) {
                // Recursive call - need to access PythonExecutor instance
                // For now, create a simple conversion
                std::visit([&result](auto&& inner_arg) {
                    using InnerT = std::decay_t<decltype(inner_arg)>;
                    if constexpr (std::is_same_v<InnerT, int>) {
                        result.append(py::int_(inner_arg));
                    } else if constexpr (std::is_same_v<InnerT, double>) {
                        result.append(py::float_(inner_arg));
                    } else if constexpr (std::is_same_v<InnerT, bool>) {
                        result.append(py::bool_(inner_arg));
                    } else if constexpr (std::is_same_v<InnerT, std::string>) {
                        result.append(py::str(inner_arg));
                    } else if constexpr (std::is_same_v<InnerT, std::monostate>) {
                        result.append(py::none());
                    } else {
                        result.append(py::str("<complex>"));
                    }
                }, item->data);
            }
            return result;
        }
        else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>) {
            // Dictionary → Python dict
            py::dict result;
            for (const auto& [key, value] : arg) {
                // Simple conversion for nested values
                std::visit([&result, &key](auto&& inner_arg) {
                    using InnerT = std::decay_t<decltype(inner_arg)>;
                    if constexpr (std::is_same_v<InnerT, int>) {
                        result[py::str(key)] = py::int_(inner_arg);
                    } else if constexpr (std::is_same_v<InnerT, double>) {
                        result[py::str(key)] = py::float_(inner_arg);
                    } else if constexpr (std::is_same_v<InnerT, bool>) {
                        result[py::str(key)] = py::bool_(inner_arg);
                    } else if constexpr (std::is_same_v<InnerT, std::string>) {
                        result[py::str(key)] = py::str(inner_arg);
                    } else if constexpr (std::is_same_v<InnerT, std::monostate>) {
                        result[py::str(key)] = py::none();
                    } else {
                        result[py::str(key)] = py::str("<complex>");
                    }
                }, value->data);
            }
            return result;
        }
        else {
            return py::str("<unknown>");
        }
    }, val->data);
}

// ============================================================================
// Type Conversion: Python Object → NAAb Value
// ============================================================================

std::shared_ptr<interpreter::Value> PythonExecutor::pythonToValue(const py::object& obj) {
    // None → null
    if (obj.is_none()) {
        return std::make_shared<interpreter::Value>();
    }

    // Boolean (check before int, since bool is subclass of int in Python)
    if (py::isinstance<py::bool_>(obj)) {
        return std::make_shared<interpreter::Value>(obj.cast<bool>());
    }

    // Integer
    if (py::isinstance<py::int_>(obj)) {
        return std::make_shared<interpreter::Value>(obj.cast<int>());
    }

    // Float
    if (py::isinstance<py::float_>(obj)) {
        return std::make_shared<interpreter::Value>(obj.cast<double>());
    }

    // String
    if (py::isinstance<py::str>(obj)) {
        return std::make_shared<interpreter::Value>(obj.cast<std::string>());
    }

    // List → Array
    if (py::isinstance<py::list>(obj)) {
        py::list py_list = obj.cast<py::list>();
        std::vector<std::shared_ptr<interpreter::Value>> result;

        for (const auto& item : py_list) {
            result.push_back(pythonToValue(py::cast<py::object>(item)));
        }

        return std::make_shared<interpreter::Value>(result);
    }

    // Tuple → Array
    if (py::isinstance<py::tuple>(obj)) {
        py::tuple py_tuple = obj.cast<py::tuple>();
        std::vector<std::shared_ptr<interpreter::Value>> result;

        for (const auto& item : py_tuple) {
            result.push_back(pythonToValue(py::cast<py::object>(item)));
        }

        return std::make_shared<interpreter::Value>(result);
    }

    // Dict → Dictionary
    if (py::isinstance<py::dict>(obj)) {
        py::dict py_dict = obj.cast<py::dict>();
        std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> result;

        for (const auto& item : py_dict) {
            std::string key = py::str(item.first).cast<std::string>();
            result[key] = pythonToValue(py::cast<py::object>(item.second));
        }

        return std::make_shared<interpreter::Value>(result);
    }

    // Unknown type - convert to string representation
    fmt::print("[WARN] Unknown Python type, converting to string\n");
    std::string str_repr = py::str(obj).cast<std::string>();
    return std::make_shared<interpreter::Value>(str_repr);
}

} // namespace runtime
} // namespace naab
