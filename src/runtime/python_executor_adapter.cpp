// NAAb Python Executor Adapter Implementation
// Adapts PythonCExecutor to the Executor interface

#include "naab/interpreter.h"  // Phase 2.3: MUST be first for Value definition
#include "naab/python_executor_adapter.h"
#include <fmt/core.h>

namespace naab {
namespace runtime {

PyExecutorAdapter::PyExecutorAdapter() {
    // PythonCExecutor uses pure C API - no pybind11, no CFI crashes
    // No need to acquire GIL here - PythonCExecutor acquires it when needed
    executor_ = std::make_unique<PythonCExecutor>();
}

bool PyExecutorAdapter::execute(const std::string& code) {
    // PythonCExecutor handles GIL acquisition internally
    try {
        executor_->execute(code);
        return true;
    } catch (const std::exception& e) {
        fmt::print("[PY ADAPTER ERROR] {}\n", e.what());
        return false;
    }
}

// Phase 2.3: Execute code and return result value
std::shared_ptr<interpreter::Value> PyExecutorAdapter::executeWithReturn(
    const std::string& code) {
    // PythonCExecutor handles GIL acquisition internally
    try {
        auto result = executor_->executeWithReturn(code);
        return result;
    } catch (const std::exception& e) {
        std::string error_msg = e.what();
        fmt::print("[PY ADAPTER ERROR] {}\n", error_msg);

        // Add hints for common Python-in-polyglot errors
        if (error_msg.find("expected an indented block") != std::string::npos) {
            fmt::print("\n  Hint: Python indentation error inside polyglot block.\n"
                       "  NAAb strips common indentation from <<python blocks.\n"
                       "  Ensure your Python code has consistent indentation:\n"
                       "    ✗ Wrong (mixed indent levels relative to NAAb code):\n"
                       "      let x = <<python\n"
                       "      if True:\n"
                       "      print('yes')  // not indented relative to 'if'\n"
                       "      >>\n"
                       "    ✓ Right (consistent Python indentation):\n"
                       "      let x = <<python\n"
                       "      if True:\n"
                       "          print('yes')  // indented under 'if'\n"
                       "      >>\n\n");
        } else if (error_msg.find("ModuleNotFoundError") != std::string::npos ||
                   error_msg.find("No module named") != std::string::npos) {
            fmt::print("\n  Hint: Python module not found inside polyglot block.\n"
                       "  - Install missing packages: pip install <package>\n"
                       "  - For complex Python logic, consider using an external .py script\n"
                       "    and calling it via <<sh: python3 script.py\n\n");
        } else if (error_msg.find("FileNotFoundError") != std::string::npos ||
                   error_msg.find("No such file or directory") != std::string::npos) {
            fmt::print("\n  Hint: File not found in Python polyglot block.\n"
                       "  - Paths in <<python blocks are relative to the working directory\n"
                       "    where naab-lang was invoked, NOT the .naab file location.\n"
                       "  - Use os.path.abspath() to verify the resolved path.\n"
                       "  - NAAb sets NAAB_INTERPRETER_PATH and NAAB_LANGUAGE_DIR env vars.\n"
                       "    Access them with: os.environ['NAAB_INTERPRETER_PATH']\n\n");
        } else if (error_msg.find("SyntaxError") != std::string::npos) {
            fmt::print("\n  Hint: Python syntax error in polyglot block.\n"
                       "  - Check for NAAb string interpolation conflicts ($ characters)\n"
                       "  - f-strings with curly braces work fine in <<python blocks\n"
                       "  - For complex Python, use an external .py script via <<sh\n\n");
        } else if (error_msg.find("NameError") != std::string::npos) {
            fmt::print("\n  Hint: Python variable not defined.\n"
                       "  - Bound variables from NAAb use <<python[var1, var2] syntax\n"
                       "  - Variables are injected as Python locals at the top of the block\n"
                       "  - Check spelling and that the variable is listed in the binding list\n\n");
        }

        return std::make_shared<interpreter::Value>();  // Return null on error
    }
}

std::shared_ptr<interpreter::Value> PyExecutorAdapter::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // PythonCExecutor handles GIL acquisition internally
    auto result = executor_->callFunction(function_name, args);
    return result;
}

bool PyExecutorAdapter::isInitialized() const {
    return executor_ != nullptr;
}

std::string PyExecutorAdapter::getCapturedOutput() {
    if (executor_) {
        return executor_->getCapturedOutput();
    }
    return "";
}

} // namespace runtime
} // namespace naab
