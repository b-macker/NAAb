// NAAb Shell Executor Implementation
// Executes shell commands using fork/execvp for separate stdout/stderr capture

#include "naab/shell_executor.h"
#include "naab/interpreter.h" // For Value definition
#include "naab/ast.h" // For ast::StructField and ast::Type
#include "naab/subprocess_helpers.h" // For execute_subprocess_with_pipes
#include "naab/sandbox.h" // For security sandbox
#include "naab/resource_limits.h" // Enterprise security: Resource limits
#include "naab/audit_logger.h" // For security audit logging
#include <cstdio>       // For popen, pclose, FILE
#include <array>        // For std::array
#include <stdexcept>    // For std::runtime_error
#include <fmt/core.h>   // For fmt::print
#include <sstream>      // For std::istringstream


namespace naab {
namespace runtime {

ShellExecutor::ShellExecutor() {
    // Shell executor initialized (silent)
}

ShellExecutor::~ShellExecutor() {
}

bool ShellExecutor::execute(const std::string& code) {
    return runCommand(code);
}

// Phase 2.3: Execute command and return struct with {exit_code, stdout, stderr}
std::shared_ptr<naab::interpreter::Value> ShellExecutor::executeWithReturn(
    const std::string& code) {

    // Enterprise Security: Install signal handlers for resource limits (once)
    if (!security::ResourceLimiter::isInitialized()) {
        security::ResourceLimiter::installSignalHandlers();
    }

    // Enterprise Security: Get timeout from sandbox config (if active)
    unsigned int timeout = 30;  // Default: 30 seconds
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (sandbox) {
        timeout = sandbox->getConfig().max_cpu_seconds;
    }

    // Enterprise Security: Apply timeout for shell execution
    security::ScopedTimeout scoped_timeout(timeout);

    // Enterprise Security: FAIL-CLOSED - Check if system command execution is allowed
    // Block by default if no sandbox or if sandbox denies execution
    bool execution_allowed = false;
    if (sandbox) {
        // Check both the allow_exec flag and SYS_EXEC capability
        execution_allowed = sandbox->getConfig().allow_exec &&
                           sandbox->getConfig().hasCapability(security::Capability::SYS_EXEC);
    }
    // If no sandbox, default to DENY (fail-closed security)

    if (!execution_allowed) {
        throw std::runtime_error(
            "Security: Shell command execution denied by sandbox\n\n"
            "  Shell blocks can execute arbitrary system commands.\n"
            "  For security, shell execution is disabled by default.\n\n"
            "  To enable (not recommended for untrusted code):\n"
            "    naab-lang run --sandbox-level unrestricted script.naab\n"
        );
    }

    // Executing with return (silent)

    std::string stdout_output;
    std::string stderr_output;
    int exit_code;

    // Execute command and capture output
    // Always use shell to properly handle operators like &&, ||, |, ;, etc.
    // Check if command contains shell operators or special characters
    bool needs_shell = (code.find('\n') != std::string::npos ||
                       code.find("&&") != std::string::npos ||
                       code.find("||") != std::string::npos ||
                       code.find('|') != std::string::npos ||
                       code.find(';') != std::string::npos ||
                       code.find('>') != std::string::npos ||
                       code.find('<') != std::string::npos ||
                       code.find('&') != std::string::npos ||
                       code.find('$') != std::string::npos ||
                       code.find('`') != std::string::npos ||
                       code.find('\'') != std::string::npos ||
                       code.find('"') != std::string::npos);

    if (needs_shell) {
        // Use shell to interpret command
        std::vector<std::string> args = {"-c", code};
        exit_code = execute_subprocess_with_pipes(
            "sh", args, stdout_output, stderr_output, nullptr
        );
    } else {
        // Simple command - execute directly for better performance
        std::istringstream iss(code);
        std::string cmd_path;
        iss >> cmd_path;
        std::vector<std::string> args;
        std::string arg;
        while (iss >> arg) {
            args.push_back(arg);
        }
        exit_code = execute_subprocess_with_pipes(
            cmd_path, args, stdout_output, stderr_output, nullptr
        );
    }

    // Print output (side effects preserved)
    if (!stdout_output.empty()) {
        fmt::print("{}", stdout_output);
    }
    if (!stderr_output.empty()) {
        fmt::print("[Shell stderr]: {}", stderr_output);

        // Helper hints for common shell errors
        if (stderr_output.find("No such file or directory") != std::string::npos) {
            if (stderr_output.find("naab-lang") != std::string::npos ||
                stderr_output.find("naab") != std::string::npos) {
                fmt::print("\n\n  Hint: Can't find naab-lang? Use the environment variable instead of a hardcoded path:\n"
                           "    Python: naab_path = os.environ['NAAB_INTERPRETER_PATH']\n"
                           "    Shell:  $NAAB_INTERPRETER_PATH\n"
                           "  These are automatically set by NAAb at startup.\n"
                           "  NAAB_LANGUAGE_DIR points to the language root directory.\n\n");
            } else if (stderr_output.find("python") != std::string::npos ||
                       stderr_output.find("Python") != std::string::npos) {
                fmt::print("\n\n  Hint: Python script not found. Check the path is relative to the working directory,\n"
                           "  not relative to the .naab file. Use absolute paths or os.path.abspath().\n\n");
            } else {
                fmt::print("\n\n  Hint: File or command not found. In <<sh blocks, paths are relative to\n"
                           "  the working directory where naab-lang was invoked, not the .naab file location.\n\n");
            }
        } else if (stderr_output.find("Permission denied") != std::string::npos) {
            fmt::print("\n\n  Hint: Permission denied. Make sure the script is executable:\n"
                       "    chmod +x <script_path>\n\n");
        } else if (stderr_output.find("command not found") != std::string::npos) {
            fmt::print("\n\n  Hint: Command not found. Check that the program is installed and in your PATH.\n"
                       "  For NAAb interpreter, use: $NAAB_INTERPRETER_PATH\n\n");
        } else if (stderr_output.find("Module not found") != std::string::npos ||
                   stderr_output.find("Failed to load module") != std::string::npos) {
            fmt::print("\n\n  Hint: NAAb module not found error.\n"
                       "  The --path flag sets the search directory for 'use' imports.\n"
                       "  If your script has 'use modules.risk_engine', NAAb looks for:\n"
                       "    <path>/modules/risk_engine.naab\n\n"
                       "  So --path should be the PARENT directory of 'modules/', not the modules dir itself.\n"
                       "  Example: if modules/ is at /project/modules/risk_engine.naab:\n"
                       "    naab-lang run script.naab --path /project\n"
                       "  NOT:\n"
                       "    naab-lang run script.naab --path /project/modules\n\n");
        } else if (stderr_output.find("Expecting value: line 1 column 1") != std::string::npos) {
            fmt::print("\n\n  Hint: Python json.loads()/json.load() received an empty string.\n\n"
                       "  IMPORTANT: If you have a broad 'except Exception' block, the error may NOT be\n"
                       "  from the json.load() you think! It could be a DIFFERENT json.loads() call later\n"
                       "  in your script (e.g., parsing subprocess output). Print the full traceback to find\n"
                       "  the exact line:\n"
                       "    except Exception as e:\n"
                       "        import traceback; traceback.print_exc()  # shows EXACT line number\n\n"
                       "  Common causes:\n"
                       "  1. A subprocess returned empty output, and you called json.loads() on it\n"
                       "  2. f.read() for debugging exhausted the file handle before json.load(f)\n"
                       "  3. A different json.loads() call in your script is the one actually failing\n\n"
                       "  Fix: Replace 'except Exception as e: print(e)' with 'traceback.print_exc()'\n"
                       "  to see which line actually threw the error.\n\n");
        } else if (stderr_output.find("JSONDecodeError") != std::string::npos ||
                   stderr_output.find("json.decoder") != std::string::npos) {
            fmt::print("\n\n  Hint: Python JSON decode error in subprocess.\n"
                       "  - Check that the data being parsed is valid JSON (no trailing commas, no comments)\n"
                       "  - If reading from a file, ensure the file handle isn't exhausted (don't call f.read() twice)\n"
                       "  - If parsing subprocess output, check for non-JSON text mixed in (warnings, debug prints)\n\n");
        } else if (stderr_output.find("TypeError") != std::string::npos &&
                   stderr_output.find("NoneType") != std::string::npos) {
            fmt::print("\n\n  Hint: Python TypeError with NoneType — a function returned None unexpectedly.\n"
                       "  - Check that all functions have explicit return statements\n"
                       "  - A failed operation (file read, API call) may have returned None\n"
                       "  - Add 'if result is None' checks before using return values\n\n");
        } else if (stderr_output.find("KeyError") != std::string::npos) {
            fmt::print("\n\n  Hint: Python KeyError — a dictionary key doesn't exist.\n"
                       "  - Use dict.get('key', default) instead of dict['key'] to avoid crashes\n"
                       "  - Print the dict keys to verify the structure: print(list(data.keys()))\n\n");
        } else if (stderr_output.find("NameError") != std::string::npos) {
            fmt::print("\n\n  Hint: Python NameError — a variable or function is not defined.\n"
                       "  - Check for typos in variable names\n"
                       "  - Make sure the variable is defined before it's used (not in a different function scope)\n"
                       "  - If using f-strings to generate code, the variable may be in the template\n"
                       "    but never assigned in the function that runs the generated code\n"
                       "  - Use 'import traceback; traceback.print_exc()' to see the exact line\n\n");
        } else if (stderr_output.find("Traceback") != std::string::npos &&
                   stderr_output.find("ImportError") != std::string::npos) {
            fmt::print("\n\n  Hint: Python ImportError in subprocess.\n"
                       "  - Install missing packages: pip install <package>\n"
                       "  - Check that the Python version matches (python3 vs python)\n\n");
        } else if ((stderr_output.find("exit code 1") != std::string::npos ||
                    stderr_output.find("exit code: 1") != std::string::npos ||
                    stderr_output.find("returncode=1") != std::string::npos) &&
                   (stderr_output.find("naab") != std::string::npos ||
                    stderr_output.find("NAAb") != std::string::npos)) {
            fmt::print("\n\n  Hint: A subprocess calling naab-lang failed with exit code 1.\n"
                       "  NAAb prints error messages to STDOUT, not stderr.\n"
                       "  When using subprocess.run(), check result.stdout for the error:\n\n"
                       "    result = subprocess.run(cmd, capture_output=True, text=True)\n"
                       "    if result.returncode != 0:\n"
                       "        print('STDOUT:', result.stdout)   # <-- NAAb errors are HERE\n"
                       "        print('STDERR:', result.stderr)   # may be empty\n\n"
                       "  Common NAAb script errors:\n"
                       "  - Parse error: mismatched braces {{ }} or missing 'main {{ }}' block\n"
                       "  - Module not found: check 'use' paths are relative to working directory\n"
                       "  - Use env.get_args() to read command-line arguments in NAAb scripts\n\n");
        } else if (stderr_output.find("failed") != std::string::npos &&
                   stderr_output.find("exit code") != std::string::npos &&
                   stderr_output.find("stderr") == std::string::npos) {
            // Generic subprocess failure with no stderr content reported
            fmt::print("\n\n  Hint: A subprocess failed but stderr appears empty.\n"
                       "  Many programs (including naab-lang) print errors to stdout, not stderr.\n"
                       "  Check stdout for error details:\n"
                       "    result = subprocess.run(cmd, capture_output=True, text=True)\n"
                       "    print('stdout:', result.stdout)  # check here for errors\n\n");
        }
    }

    if (exit_code != 0) {
        // Command failed (silent - error will be in stderr)
    }

    // Trim trailing newlines from outputs
    auto trim_trailing_newline = [](std::string& str) {
        while (!str.empty() && str.back() == '\n') {
            str.pop_back();
        }
    };
    trim_trailing_newline(stdout_output);
    trim_trailing_newline(stderr_output);

    // For simple successful cases (exit_code == 0 and no stderr), return just stdout
    // This makes shell execution more intuitive for common cases
    if (exit_code == 0 && stderr_output.empty()) {
        // Try to parse stdout as a number for better interop
        // If it's a valid integer, return as int; otherwise return as string
        if (!stdout_output.empty()) {
            try {
                // Try parsing as integer
                size_t pos;
                int int_val = std::stoi(stdout_output, &pos);
                // Check if entire string was consumed (it's a pure integer)
                if (pos == stdout_output.length()) {
                    return std::make_shared<interpreter::Value>(int_val);
                }
            } catch (...) {
                // Not an integer, fall through to return as string
            }
        }
        // Return stdout as string
        return std::make_shared<interpreter::Value>(stdout_output);
    }

    // For error cases or when stderr is present, return full ShellResult struct
    // struct ShellResult { exit_code: int, stdout: string, stderr: string }
    std::vector<ast::StructField> fields;
    fields.push_back(ast::StructField{"exit_code", ast::Type::makeInt(), std::nullopt});
    fields.push_back(ast::StructField{"stdout", ast::Type::makeString(), std::nullopt});
    fields.push_back(ast::StructField{"stderr", ast::Type::makeString(), std::nullopt});

    auto struct_def = std::make_shared<interpreter::StructDef>("ShellResult", std::move(fields));

    // Create struct instance
    auto struct_value = std::make_shared<interpreter::StructValue>("ShellResult", struct_def);

    // Set field values
    struct_value->field_values[0] = std::make_shared<interpreter::Value>(exit_code);
    struct_value->field_values[1] = std::make_shared<interpreter::Value>(stdout_output);
    struct_value->field_values[2] = std::make_shared<interpreter::Value>(stderr_output);

    // Return struct wrapped in Value
    return std::make_shared<interpreter::Value>(struct_value);
}

std::shared_ptr<naab::interpreter::Value> ShellExecutor::callFunction(
    const std::string& function_name,
    const std::vector<std::shared_ptr<naab::interpreter::Value>>& args) {
    
    if (function_name == "exec" && !args.empty()) {
        // Handle string argument
        if (auto* str_val = std::get_if<std::string>(&args[0]->data)) {
            bool success = runCommand(*str_val);
            return std::make_shared<naab::interpreter::Value>(success);
        }
        // Handle integer argument (convert to string)
        if (auto* int_val = std::get_if<int>(&args[0]->data)) {
             bool success = runCommand(std::to_string(*int_val));
             return std::make_shared<naab::interpreter::Value>(success);
        }
    }
    
    throw std::runtime_error("Shell executor only supports 'exec(command_string)'");
}

std::string ShellExecutor::getCapturedOutput() {
    std::string output = stdout_buffer_.getAndClear();
    std::string error_output = stderr_buffer_.getAndClear();
    if (!error_output.empty()) {
        output += "\n[Shell stderr]: " + error_output;
    }
    return output;
}

bool ShellExecutor::runCommand(const std::string& command) {
    // Check sandbox permissions for command execution
    auto* sandbox = naab::security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(naab::security::Capability::BLOCK_CALL)) {
        fmt::print("[ERROR] Sandbox violation: Shell execution denied\n");
        sandbox->logViolation("executeShell", command, "BLOCK_CALL capability required");
        throw std::runtime_error("Shell execution denied by sandbox");
    }

    // Executing command (silent)

    std::string stdout_buffer_local;
    std::string stderr_buffer_local;
    int exit_code;

    // Check if this is a multi-line script (contains newline)
    if (command.find('\n') != std::string::npos) {
        // Execute as a shell script using sh -c
        std::vector<std::string> args = {"-c", command};
        exit_code = execute_subprocess_with_pipes(
            "sh", args, stdout_buffer_local, stderr_buffer_local, nullptr
        );
    } else {
        // Parse the command string into command and arguments
        // For simplicity, assume the first word is the command and the rest are args
        std::istringstream iss(command);
        std::string cmd_path;
        iss >> cmd_path;
        std::vector<std::string> args;
        std::string arg;
        while (iss >> arg) {
            args.push_back(arg);
        }

        exit_code = execute_subprocess_with_pipes(
            cmd_path, args, stdout_buffer_local, stderr_buffer_local, nullptr
        );
    }
    
    // Append to internal buffers
    stdout_buffer_.append(stdout_buffer_local);
    stderr_buffer_.append(stderr_buffer_local);

    bool success = (exit_code == 0);
    
    if (success) {
        // Shell command executed (silent)
    } else {
        fmt::print("[ERROR] Shell command failed with code {} (captured stderr: \'{}\')\n", exit_code, stderr_buffer_local);
    }
    return success;
}

} // namespace runtime
} // namespace naab
