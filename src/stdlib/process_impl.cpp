//
// NAAb Standard Library - Process Module
// Subprocess management: run, exit, kill, getpid
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/safe_math.h"
#include "naab/subprocess_helpers.h"
#include "naab/sandbox.h"
#include "naab/governance.h"
#include "naab/platform.h"
#include "naab/limits.h"
#include <unordered_set>
#include <unordered_map>
#include <stdexcept>
#include <cstdlib>
#include <cctype>

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

namespace {

// Recognise `<interpreter> <inline-code flag> <code>` and return the code with
// the governance language name. The command may carry a directory, a Windows
// .exe suffix or a version suffix (python3.12, /usr/bin/node). Short flags may
// be clustered (`bash -ec`, `python3 -Ic`) when the code flag comes last.
bool inlineInterpreterCode(const std::string& cmd,
                           const std::vector<std::string>& argv,
                           std::string& lang, std::string& code) {
    std::string base = cmd;
    auto slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    for (auto& c : base) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".exe") == 0)
        base.resize(base.size() - 4);
    // strip a trailing version: python3.12 -> python, ruby3.2 -> ruby
    while (!base.empty() && (std::isdigit(static_cast<unsigned char>(base.back())) || base.back() == '.'))
        base.pop_back();

    std::string short_flags;   // single letters that take the code as the next arg
    std::vector<std::string> long_flags;
    if (base == "python" || base == "pypy") { lang = "python"; short_flags = "c"; }
    else if (base == "node" || base == "nodejs") { lang = "javascript"; short_flags = "ep"; long_flags = {"--eval", "--print"}; }
    else if (base == "ruby") { lang = "ruby"; short_flags = "e"; }
    else if (base == "perl") { lang = "perl"; short_flags = "eE"; }
    else if (base == "php") { lang = "php"; short_flags = "r"; }
    else if (base == "bash" || base == "sh" || base == "dash" || base == "zsh" || base == "ksh") { lang = "shell"; short_flags = "c"; }
    else return false;

    for (size_t i = 0; i + 1 < argv.size(); ++i) {
        const std::string& a = argv[i];
        bool hit = false;
        for (const auto& lf : long_flags) if (a == lf) hit = true;
        if (!hit && a.size() >= 2 && a[0] == '-' && a[1] != '-' &&
            short_flags.find(a.back()) != std::string::npos) hit = true;
        if (hit) { code = argv[i + 1]; return true; }
    }
    // `node --eval=CODE`
    for (const auto& a : argv)
        for (const auto& lf : long_flags)
            if (a.rfind(lf + "=", 0) == 0) { code = a.substr(lf.size() + 1); return true; }
    return false;
}

} // namespace

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
                "  The project owner can adjust the sandbox level in the project configuration.\n"
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

        // F17: the SANDBOX check above carries SYS_EXEC and knows nothing about
        // capabilities.shell.blocked_commands, which was enforced only by
        // scanning <<shell>> source text. Measured: `<<shell>> whoami` blocked
        // while this ran, same config, same command.
        //
        // Checked AFTER argv is built and against the whole command line, so a
        // blocked token cannot be smuggled past as an argument
        // (process.run("sh", ["-c", "whoami"])). The engine uses the same
        // substring match the polyglot path uses -- one config key must not
        // have two matchers.
        if (auto* gov = governance::GovernanceEngine::getCurrent()) {
            std::string full_cmd = cmd;
            for (const auto& a : argv_vec) full_cmd += " " + a;
            std::string gerr = gov->checkShellCommandAllowed(full_cmd);
            if (!gerr.empty()) throw std::runtime_error(gerr);

            // Inline interpreter code (python3 -c, node -e, sh -c, ...) is a
            // polyglot block by another name. It used to skip every code check
            // a <<python>> block or codegen.run() gets: measured, the same
            // `except Exception: return 0` was a HARD block in both of those
            // and ran silently here. Route it through the same check.
            if (gov->isActive()) {
                std::string lang, code;
                if (inlineInterpreterCode(cmd, argv_vec, lang, code)) {
                    std::string berr = gov->checkPolyglotBlock(
                        lang, code, "<process.run:" + lang + ">", 0);
                    if (!berr.empty()) throw std::runtime_error(berr);
                }
            }
        }

        std::string stdout_str, stderr_str;
        auto containment = naab::runtime::SubprocessContainment::fromCurrentSandbox(cmd);
        int exit_code = naab::runtime::execute_subprocess_with_pipes(
            cmd, argv_vec, stdout_str, stderr_str, nullptr, &containment
        );

        std::unordered_map<std::string, interpreter::NaabVal> result;
        result["exit_code"] = interpreter::NaabVal::makeInt(exit_code);
        result["stdout"]    = interpreter::NaabVal::makeString(stdout_str);
        result["stderr"]    = interpreter::NaabVal::makeString(stderr_str);
        return interpreter::NaabVal::makeDict(std::move(result));
    }

    if (function_name == "exit") {
        // Sandbox gate: process.exit() requires SYS_EXEC capability
        auto* sb = naab::security::ScopedSandbox::getCurrent();
        if (sb && !sb->getConfig().hasCapability(naab::security::Capability::SYS_EXEC)) {
            sb->logViolation("process.exit", "exit",
                "SYS_EXEC capability required");
            throw std::runtime_error(
                "Security: process.exit() denied by sandbox\n\n"
                "  Use 'return' at end of main{} to exit cleanly.\n"
            );
        }
        int code = 0;
        if (!args.empty()) {
            if (args[0].isInt()) {
                code = args[0].asInt();
            } else if (args[0].isDouble()) {
                code = naab::math::safeDoubleToInt(args[0].asDouble());
            }
        }
        // V-DOS-014: Throw ExitException instead of calling std::exit() directly.
        // CLI mode catches this and calls _exit(code); embedded/server mode
        // treats it as a normal exception, preventing host process termination.
        throw naab::limits::ExitException(code);
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
            pid = naab::math::safeDoubleToInt(args[0].asDouble());
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

        // Sandbox check: SYS_EXEC required (same as process.run)
        auto* sb = naab::security::ScopedSandbox::getCurrent();
        if (sb && !sb->canExecuteCommand("kill")) {
            sb->logViolation("process.kill", std::to_string(pid), "SYS_EXEC capability required");
            throw std::runtime_error(
                "Security: process.kill() denied by sandbox\n\n"
                "  PID: " + std::to_string(pid) + "\n\n"
                "  process.kill() requires SYS_EXEC capability.\n"
                "  The project owner can adjust the sandbox level in the project configuration.\n"
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
