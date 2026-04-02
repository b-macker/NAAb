//
// NAAb Standard Library - Process Module
// Subprocess management: run, exit, kill, getpid
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/subprocess_helpers.h"
#include "naab/sandbox.h"
#include "naab/platform.h"
#include <unordered_set>
#include <unordered_map>
#include <stdexcept>
#include <cstdlib>

#ifndef _WIN32
#  include <csignal>
#  include <unistd.h>
#else
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace naab {
namespace stdlib {

bool ProcessModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "run", "exit", "kill", "getpid"
    };
    return functions.count(name) > 0;
}

interpreter::NaabVal ProcessModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    if (function_name == "run") {
        if (args.empty()) {
            throw std::runtime_error(
                "process.run() requires at least 1 argument: (command, [args])\n\n"
                "  Example: let r = process.run(\"echo\", [\"hello\"])\n"
                "  Returns: {\"exit_code\": 0, \"stdout\": \"hello\\n\", \"stderr\": \"\"}\n"
            );
        }

        std::string cmd = args[0].toString();

        // Sandbox check: SYS_EXEC required
        auto* sb = naab::security::ScopedSandbox::getCurrent();
        if (sb && !sb->canExecuteCommand(cmd)) {
            sb->logViolation("process.run", cmd, "SYS_EXEC capability required");
            throw std::runtime_error(
                "Security: process.run() denied by sandbox\n\n"
                "  Command: " + cmd + "\n\n"
                "  process.run() requires SYS_EXEC capability.\n"
                "  Use --sandbox-level elevated or higher.\n"
            );
        }

        // Build argv vector: [arg1, arg2, ...]
        // execute_subprocess_with_pipes prepends cmd as argv[0] itself —
        // do NOT push cmd here or it appears twice in the argument list.
        std::vector<std::string> argv_vec;
        if (args.size() > 1 && args[1].isList()) {
            for (const auto& item : args[1].asListConst()) {
                argv_vec.push_back(item.toString());
            }
        }

        std::string stdout_str, stderr_str;
        int exit_code = naab::runtime::execute_subprocess_with_pipes(
            cmd, argv_vec, stdout_str, stderr_str, nullptr
        );

        std::unordered_map<std::string, interpreter::NaabVal> result;
        result["exit_code"] = interpreter::NaabVal::makeInt(exit_code);
        result["stdout"]    = interpreter::NaabVal::makeString(stdout_str);
        result["stderr"]    = interpreter::NaabVal::makeString(stderr_str);
        return interpreter::NaabVal::makeDict(std::move(result));
    }

    if (function_name == "exit") {
        int code = 0;
        if (!args.empty()) {
            if (args[0].isInt()) {
                code = args[0].asInt();
            } else if (args[0].isDouble()) {
                code = static_cast<int>(args[0].asDouble());
            }
        }
        std::exit(code);
        // NOTREACHED
    }

    if (function_name == "kill") {
        if (args.empty()) {
            throw std::runtime_error(
                "process.kill() requires 1 argument: (pid)\n\n"
                "  Example: process.kill(1234)\n"
            );
        }
        int pid = 0;
        if (args[0].isInt()) {
            pid = args[0].asInt();
        } else if (args[0].isDouble()) {
            pid = static_cast<int>(args[0].asDouble());
        } else {
            throw std::runtime_error(
                "process.kill(): pid must be an integer, got " + args[0].getTypeName()
            );
        }
        if (pid <= 0) {
            throw std::runtime_error(
                "process.kill(): pid must be a positive integer, got " + std::to_string(pid)
            );
        }
#ifndef _WIN32
        if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0) {
            throw std::runtime_error(
                "process.kill(): failed to send SIGTERM to PID " + std::to_string(pid) +
                " (process may not exist or permission denied)"
            );
        }
#else
        HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (!h) {
            throw std::runtime_error(
                "process.kill(): failed to open PID " + std::to_string(pid) +
                " (error " + std::to_string(::GetLastError()) + ")"
            );
        }
        BOOL ok = ::TerminateProcess(h, 1);
        ::CloseHandle(h);
        if (!ok) {
            throw std::runtime_error(
                "process.kill(): TerminateProcess failed for PID " + std::to_string(pid) +
                " (error " + std::to_string(::GetLastError()) + ")"
            );
        }
#endif
        return interpreter::NaabVal::makeNull();
    }

    if (function_name == "getpid") {
        return interpreter::NaabVal::makeInt(naab::platform::getpid());
    }

    throw std::runtime_error(
        "process." + function_name + "(): unknown function\n\n"
        "  Available: run, exit, kill, getpid\n"
    );
}

} // namespace stdlib
} // namespace naab
