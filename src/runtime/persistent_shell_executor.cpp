// NAAb Persistent Shell Executor Implementation
// Bash process kept alive via stdin/stdout pipes with sentinel protocol

#include "naab/persistent_shell_executor.h"
#include "naab/interpreter.h"  // For Value, StructDef, StructValue
#include "naab/ast.h"          // For ast::StructField, ast::Type
#include <climits>
#include <random>  // here-document terminator nonce
#include <fmt/core.h>

namespace naab {
namespace runtime {

PersistentShellExecutor::PersistentShellExecutor()
    : PersistentProcessExecutor("shell", "bash", {"--norc", "--noprofile"}) {
}

std::string PersistentShellExecutor::getSentinel() const {
    return "__NAAB_BLOCK_DONE__";
}

std::string PersistentShellExecutor::getStartupCode() const {
    return "set +o verbose\n"
           "set +o xtrace\n"
           "echo '__NAAB_BLOCK_DONE__'\n";
}

std::string PersistentShellExecutor::getExitCommand() const {
    return "exit 0\n";
}

std::string PersistentShellExecutor::wrapCodeForExecution(const std::string& code) const {
    // The block reaches bash as ONE unit: read verbatim into a variable from a
    // quoted here-document (builtin `read`, no expansion, no fork), then eval'd.
    //
    // It used to be written to bash's stdin raw, followed by the exit-code and
    // sentinel lines. A block that is INCOMPLETE input -- an unclosed quote,
    // `(`, `if ... then` -- made bash read those lines as part of the open
    // construct, so the sentinel never came and the block waited out the
    // executor's timeout (30 s, or --timeout) before failing. Under eval the
    // same block fails at once ("unexpected end of file", exit 2) and the
    // sentinel lines still run. eval runs in this shell, so variables and
    // functions a block defines persist exactly as before.
    //
    // The terminator carries a random suffix per call, so a block cannot end
    // the here-document by accident.
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const std::string eof = "__NAAB_CODE_EOF_" + std::to_string(rng()) + "__";
    return "IFS= read -r -d '' __naab_code <<'" + eof + "'\n" +
           code + "\n" +
           eof + "\n"
           "eval \"$__naab_code\"\n"
           "__naab_exit=$?\n"
           "echo \"__NAAB_EXIT__:${__naab_exit}\"\n"
           "echo '__NAAB_BLOCK_DONE__'\n";
}

interpreter::NaabVal PersistentShellExecutor::parseOutput(
    const std::string& stdout_text, const std::string& stderr_text,
    int /* implicit_exit_code */) const {

    // Extract exit code from __NAAB_EXIT__:<N> line
    int exit_code = 0;
    std::string clean_stdout = stdout_text;

    // Find and extract the exit code line
    std::string exit_prefix = "__NAAB_EXIT__:";
    auto pos = clean_stdout.rfind(exit_prefix);
    if (pos != std::string::npos) {
        // Parse exit code
        auto line_end = clean_stdout.find('\n', pos);
        std::string exit_str;
        if (line_end != std::string::npos) {
            exit_str = clean_stdout.substr(pos + exit_prefix.size(),
                                           line_end - pos - exit_prefix.size());
        } else {
            exit_str = clean_stdout.substr(pos + exit_prefix.size());
        }

        // Trim whitespace
        while (!exit_str.empty() && (exit_str.back() == '\r' || exit_str.back() == ' ')) {
            exit_str.pop_back();
        }

        try {
            exit_code = std::stoi(exit_str);
        } catch (...) {
            exit_code = 1;
        }

        // Remove the __NAAB_EXIT__ line from stdout
        if (line_end != std::string::npos) {
            clean_stdout.erase(pos, line_end - pos + 1);
        } else {
            clean_stdout.erase(pos);
        }
    }

    // Trim trailing newlines
    while (!clean_stdout.empty() && clean_stdout.back() == '\n') {
        clean_stdout.pop_back();
    }

    // For successful cases (exit_code == 0, no stderr): return stdout as string
    // Shell output is always a string. Users can explicitly convert with int(), float(), etc.
    // Do NOT perform implicit type coercion — it leads to subtle bugs when numeric output
    // is unexpectedly converted to int/float (e.g., date +%Y → 2026 as int, then string.trim() fails)
    if (exit_code == 0 && stderr_text.empty()) {
        // Empty result -> null
        if (clean_stdout.empty()) {
            return interpreter::NaabVal::makeNull();
        }

        // Return as string (always)
        return interpreter::NaabVal::makeString(clean_stdout);
    }

    // For error cases or stderr present: return ShellResult struct
    // Same pattern as shell_executor.cpp:291-308
    std::string trimmed_stderr = stderr_text;
    while (!trimmed_stderr.empty() && trimmed_stderr.back() == '\n') {
        trimmed_stderr.pop_back();
    }

    std::vector<ast::StructField> fields;
    fields.push_back(ast::StructField{"exit_code", ast::Type::makeInt(), std::nullopt});
    fields.push_back(ast::StructField{"stdout", ast::Type::makeString(), std::nullopt});
    fields.push_back(ast::StructField{"stderr", ast::Type::makeString(), std::nullopt});

    auto struct_def = std::make_shared<interpreter::StructDef>("ShellResult", std::move(fields));
    auto struct_value = std::make_shared<interpreter::StructValue>("ShellResult", struct_def);

    struct_value->field_values[0] = interpreter::NaabVal::makeInt(exit_code);
    struct_value->field_values[1] = interpreter::NaabVal::makeString(clean_stdout);
    struct_value->field_values[2] = interpreter::NaabVal::makeString(trimmed_stderr);

    return interpreter::NaabVal::makeStruct(struct_value);
}

} // namespace runtime
} // namespace naab
