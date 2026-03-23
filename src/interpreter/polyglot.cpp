// NAAb Interpreter — Polyglot Execution
// Split from interpreter.cpp for maintainability
//
// Contains: injectDeclarationsAfterHeaders, visit(InlineCodeExpr),
//           VariableSnapshot::capture, executePolyglotGroupParallel,
//           serializeValueForLanguage

#include "naab/interpreter.h"
#include "naab/logger.h"
#include "naab/language_registry.h"
#include "naab/shell_executor.h"
#include "naab/sandbox.h"
#include "naab/resource_limits.h"
#include "naab/source_mapper.h"
#include "naab/json_result_parser.h"
#include "naab/polyglot_dependency_analyzer.h"
#include "naab/polyglot_async_executor.h"
#include <fmt/core.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <thread>
#include <future>
#include <chrono>

namespace naab {
namespace interpreter {

// File-local helper (duplicated from interpreter.cpp — static linkage)
static std::string getTypeName(const std::shared_ptr<Value>& val) {
    return std::visit([](auto&& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int>) { return "int"; }
        else if constexpr (std::is_same_v<T, double>) { return "float"; }
        else if constexpr (std::is_same_v<T, bool>) { return "bool"; }
        else if constexpr (std::is_same_v<T, std::string>) { return "string"; }
        else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<Value>>>) { return "array"; }
        else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<Value>>>) { return "dict"; }
        else if constexpr (std::is_same_v<T, std::shared_ptr<FunctionValue>>) { return "function"; }
        else if constexpr (std::is_same_v<T, std::shared_ptr<StructValue>>) { return "struct"; }
        else if constexpr (std::is_same_v<T, std::shared_ptr<FutureValue>>) { return "future"; }
        else if constexpr (std::is_same_v<T, std::monostate>) { return "null"; }
        return "unknown";
    }, val->data);
}


// Phase 12: Header-aware injection for languages that require specific first lines
std::string Interpreter::injectDeclarationsAfterHeaders(
    const std::string& declarations, const std::string& code, const std::string& language) {

    if (declarations.empty()) return code;

    std::vector<std::string> lines;
    std::istringstream stream(code);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    // Find the insertion point after all header lines
    int insert_after = -1;  // -1 means prepend (no headers found)
    bool in_block_import = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string trimmed = lines[i];
        // Trim leading whitespace
        size_t first_non_space = trimmed.find_first_not_of(" \t");
        if (first_non_space != std::string::npos) {
            trimmed = trimmed.substr(first_non_space);
        } else {
            // Empty/whitespace line — skip but don't break header scanning
            if (in_block_import) {
                insert_after = static_cast<int>(i);
            }
            continue;
        }

        if (language == "go") {
            // Go: package declaration must be first, then imports
            if (trimmed.substr(0, 8) == "package ") {
                insert_after = static_cast<int>(i);
                continue;
            }
            if (trimmed.substr(0, 7) == "import " && trimmed.find("(") != std::string::npos) {
                // Block import: import (
                in_block_import = true;
                insert_after = static_cast<int>(i);
                continue;
            }
            if (in_block_import) {
                insert_after = static_cast<int>(i);
                if (trimmed[0] == ')') {
                    in_block_import = false;
                }
                continue;
            }
            if (trimmed.substr(0, 7) == "import ") {
                // Single import
                insert_after = static_cast<int>(i);
                continue;
            }
            // If we haven't seen any header yet and this is a non-header line, stop scanning
            if (insert_after >= 0) break;
            // If no headers at all, stop immediately
            break;
        } else if (language == "php") {
            // PHP: <?php tag must be first
            if (trimmed.substr(0, 5) == "<?php" || trimmed.substr(0, 2) == "<?") {
                insert_after = static_cast<int>(i);
                continue;
            }
            break;
        } else if (language == "typescript" || language == "ts") {
            // TypeScript: import statements at top
            if (trimmed.substr(0, 7) == "import ") {
                insert_after = static_cast<int>(i);
                continue;
            }
            break;
        } else {
            // Unknown language — no header awareness, prepend
            break;
        }
    }

    // Build result: header lines + declarations + remaining lines
    std::string result;
    if (insert_after < 0) {
        // No headers found — prepend declarations
        result = declarations + code;
    } else {
        // Insert declarations after the header lines
        for (size_t i = 0; i <= static_cast<size_t>(insert_after); ++i) {
            result += lines[i] + "\n";
        }
        result += declarations;
        for (size_t i = static_cast<size_t>(insert_after) + 1; i < lines.size(); ++i) {
            result += lines[i] + "\n";
        }
    }
    return result;
}

void Interpreter::visit(ast::InlineCodeExpr& node) {
    std::string language = node.getLanguage();
    std::string raw_code = node.getCode();

    const auto& bound_vars = node.getBoundVariables();  // Phase 2.2

    // Get the executor early (needed for object-based variable passing)
    auto& registry = runtime::LanguageRegistry::instance();
    auto* executor = registry.getExecutor(language);
    if (!executor) {
        // FIX-DX-14: Enhanced error with install guidance
        std::string msg = "No executor found for language: " + language + "\n\n";
        if (language == "nim") msg += "  Install: pkg install nim (Termux) / brew install nim / apt install nim\n";
        else if (language == "go") msg += "  Install: pkg install golang (Termux) / brew install go / apt install golang\n";
        else if (language == "python") msg += "  Ensure python3 is in PATH: pkg install python (Termux)\n";
        else if (language == "javascript" || language == "js") msg += "  Ensure node is in PATH: pkg install nodejs (Termux)\n";
        else if (language == "rust") msg += "  Install: pkg install rust (Termux) / curl https://sh.rustup.rs -sSf | sh\n";
        else if (language == "zig") msg += "  Install: brew install zig / snap install zig\n";
        else if (language == "julia") msg += "  Install: brew install julia / snap install julia\n";
        msg += "\n  Verify: which " + language + "\n";
        throw std::runtime_error(msg);
    }

    // Governance v3.0: Comprehensive polyglot block check
    if (governance_ && governance_->isActive()) {
        int line = node.getLocation().line;
        std::string err = governance_->checkPolyglotBlock(
            language, raw_code, current_file_, line, bound_vars.size());
        if (!err.empty()) throw std::runtime_error(err);

        // Check polyglot block count limit
        std::string count_err = governance_->incrementAndCheckPolyglotBlockCount();
        if (!count_err.empty()) throw std::runtime_error(count_err);

        // FIX-DX-2 + FIX-D: Taint tracking for ALL language bindings
        checkPolyglotBoundVarTaint(language, bound_vars, node.getLocation().line);
    }

    // Phase 2.2: Bind variables using string serialization
    std::string var_declarations;

    for (const auto& var_name : bound_vars) {
        // Look up variable in current environment
        if (!current_env_->has(var_name)) {
            throw std::runtime_error("Variable '" + var_name + "' not found in scope for inline code binding");
        }

        auto value = current_env_->get(var_name);

        // For all languages: use string serialization
        std::string serialized = serializeValueForLanguage(value, language);

        // FIX-DX-10: Warn when complex types bound to languages needing manual parsing
        if (value) {
            bool is_complex_type = (
                std::holds_alternative<std::vector<std::shared_ptr<Value>>>(value->data) ||
                std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data));
            if (is_complex_type) {
                if (language == "go") {
                    fmt::print(stderr, "[HINT] Binding NAAb dict/array to Go var '{}' — "
                               "use json.Unmarshal([]byte({}), &target) to parse.\n",
                               var_name, var_name);
                } else if (language == "nim") {
                    fmt::print(stderr, "[HINT] Binding NAAb dict/array to Nim var '{}' — "
                               "use parseJson({}) to access fields.\n",
                               var_name, var_name);
                }
            }
        }

        // FIX-DX-13: Hint when bound string looks like JSON (roundtrip waste)
        if (value && std::holds_alternative<std::string>(value->data)) {
            const auto& sv = std::get<std::string>(value->data);
            if (sv.size() >= 2 && (sv[0] == '{' || sv[0] == '[') &&
                (sv.back() == '}' || sv.back() == ']')) {
                fmt::print(stderr, "[HINT] Variable '{}' looks like a JSON string. "
                           "NAAb can bind dicts/arrays directly (serializes natively per language).\n"
                           "  Instead of: let s = json.stringify(data)  <<{}[s]>>\n"
                           "  Try:        <<{}[data]>>  (avoids parse/stringify roundtrip)\n",
                           var_name, language, language);
            }
        }

        if (language == "python") {
            var_declarations += var_name + " = " + serialized + "\n";
        } else if (language == "shell" || language == "sh" || language == "bash") {
            // Use export for shell variables
            var_declarations += "export " + var_name + "=" + serialized + "\n";
        } else {
            if (language == "javascript" || language == "js") {
                var_declarations += "const " + var_name + " = " + serialized + ";\n";
            } else if (language == "go") {
                // Go: const only works for primitives; use var for complex types
                bool is_complex = value && (
                    std::holds_alternative<std::vector<std::shared_ptr<Value>>>(value->data) ||
                    std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data));
                if (is_complex) {
                    var_declarations += "var " + var_name + " = " + serialized + "\n";
                } else {
                    var_declarations += "const " + var_name + " = " + serialized + "\n";
                }
            } else if (language == "rust") {
                var_declarations += "let " + var_name + " = " + serialized + ";\n";
            } else if (language == "cpp" || language == "c++") {
                var_declarations += "const auto " + var_name + " = " + serialized + ";\n";
            } else if (language == "ruby") {
                var_declarations += var_name + " = " + serialized + "\n";
            } else if (language == "csharp" || language == "cs") {
                var_declarations += "var " + var_name + " = " + serialized + ";\n";
            } else if (language == "typescript" || language == "ts") {
                var_declarations += "const " + var_name + " = " + serialized + ";\n";
            } else if (language == "php") {
                // PHP vars must come after <?php tag. Add tag once, then vars.
                if (var_declarations.find("<?php") == std::string::npos) {
                    var_declarations += "<?php\n";
                }
                var_declarations += "$" + var_name + " = " + serialized + ";\n";
            } else if (language == "nim") {
                var_declarations += "var " + var_name + " = " + serialized + "\n";
            } else if (language == "zig") {
                var_declarations += "const " + var_name + " = " + serialized + ";\n";
            } else if (language == "julia") {
                var_declarations += var_name + " = " + serialized + "\n";
            }
        }
    }

    // Phase 12: Inject naab_return() helper function per language
    // Only inject if naab_return is actually used in the code (avoids breaking IIFE wrapping)
    std::string return_type = node.getReturnType();
    bool code_uses_naab_return = (raw_code.find("naab_return") != std::string::npos);
    if (code_uses_naab_return) {
        std::string helper;
        if (language == "python") {
            if (!return_type.empty()) {
                // Python with -> JSON: naab_return prints to stdout for StringIO capture
                helper = "def naab_return(data):\n    import json as __nrj\n    print(__nrj.dumps(data) if not isinstance(data, str) else data)\n";
            } else {
                // Python without -> JSON: naab_return returns data for CPython eval capture
                helper = "def naab_return(data):\n    return data\n";
            }
        } else if (language == "javascript" || language == "js") {
            // JS/QuickJS: naab_return just returns data — IIFE wrapping makes it the return value
            helper = "function naab_return(data) { return data; }\n";
        } else if (language == "typescript" || language == "ts") {
            helper = "function naab_return(data) { return data; }\n";
        } else if (language == "ruby") {
            helper = "require 'json'\ndef naab_return(data); puts \"__NAAB_RETURN__:\" + data.to_json; end\n";
        } else if (language == "php") {
            if (var_declarations.find("<?php") == std::string::npos) {
                helper = "<?php\n";
            }
            helper += "function naab_return($data) { echo \"__NAAB_RETURN__:\" . json_encode($data) . \"\\n\"; }\n";
        } else if (language == "shell" || language == "sh" || language == "bash") {
            helper = "naab_return() { echo \"__NAAB_RETURN__:$1\"; }\n";
        } else if (language == "rust") {
            helper = "macro_rules! naab_return { ($val:expr) => { println!(\"__NAAB_RETURN__:{}\", $val); }; }\n";
        } else if (language == "go") {
            // Go's naab_return needs to be inside func main, handled by executor wrapping
            helper = ""; // Will be added inside main by the executor
        } else if (language == "cpp" || language == "c++") {
            helper = "#include <sstream>\n#define naab_return(val) do { std::ostringstream __os; __os << \"__NAAB_RETURN__:\" << (val); std::cout << __os.str() << std::endl; } while(0)\n";
        } else if (language == "csharp" || language == "cs") {
            helper = ""; // C# needs it inside the class, handled by executor wrapping
        }
        if (!helper.empty()) {
            var_declarations = helper + var_declarations;
        }
    }

    // Strip common leading whitespace from all lines
    std::vector<std::string> lines;
    std::istringstream stream(raw_code);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    // Find minimum indentation (ignoring empty lines)
    // ISS-028 Fix: Include first line in indentation calculation
    size_t min_indent = std::string::npos;
    for (size_t i = 0; i < lines.size(); ++i) {  // Include all lines
        const auto& l = lines[i];
        if (l.empty() || l.find_first_not_of(" \t") == std::string::npos) continue;
        size_t indent = l.find_first_not_of(" \t");
        if (indent < min_indent) min_indent = indent;
    }

    // Strip the common indentation from ALL lines
    std::string code;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& l = lines[i];

        if (l.empty() || l.find_first_not_of(" \t") == std::string::npos) {
            // Empty or whitespace-only line
            code += "\n";
        } else {
            // Strip common indentation from all non-empty lines
            if (min_indent != std::string::npos && l.length() > min_indent) {
                code += l.substr(min_indent) + "\n";
            } else {
                code += l + "\n";
            }
        }
    }

    // Phase 2.2/12: Inject variable declarations with header awareness
    std::string final_code;
    if (!var_declarations.empty() &&
        (language == "go" || language == "php" ||
         language == "typescript" || language == "ts")) {
        final_code = injectDeclarationsAfterHeaders(var_declarations, code, language);
    } else {
        final_code = var_declarations + code;
    }

    // Phase 12: For Python with -> JSON, wrap code to capture stdout and extract last JSON line
    if (!return_type.empty() && (language == "python")) {
        // Auto-wrap bare Python expressions in print() for -> JSON
        // Find last non-empty, non-comment, non-import line and wrap if it's a bare expression
        {
            std::istringstream iss(final_code);
            std::string line;
            std::vector<std::string> lines;
            while (std::getline(iss, line)) {
                lines.push_back(line);
            }
            for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
                std::string trimmed = lines[i];
                // Trim leading whitespace
                size_t start = trimmed.find_first_not_of(" \t");
                if (start == std::string::npos) continue;  // empty line
                trimmed = trimmed.substr(start);
                if (trimmed[0] == '#') continue;  // comment
                if (trimmed.substr(0, 7) == "import " || trimmed.substr(0, 5) == "from ") continue;
                // Already has print — no wrapping needed
                if (trimmed.substr(0, 6) == "print(" || trimmed.substr(0, 7) == "print (") break;
                // Assignment — not a bare expression
                if (trimmed.find('=') != std::string::npos && trimmed.find("==") == std::string::npos
                    && trimmed.find("!=") == std::string::npos && trimmed.find(">=") == std::string::npos
                    && trimmed.find("<=") == std::string::npos) break;
                // Control flow — not a bare expression
                if (trimmed.substr(0, 3) == "if " || trimmed.substr(0, 4) == "for "
                    || trimmed.substr(0, 6) == "while " || trimmed.substr(0, 4) == "def "
                    || trimmed.substr(0, 6) == "class " || trimmed.substr(0, 4) == "try:"
                    || trimmed.substr(0, 7) == "except:" || trimmed.substr(0, 7) == "except "
                    || trimmed.substr(0, 4) == "with ") break;
                // It's a bare expression — wrap in print()
                // Preserve leading whitespace
                std::string leading = lines[i].substr(0, start);
                lines[i] = leading + "print(" + trimmed + ")";
                break;
            }
            // Rejoin
            final_code.clear();
            for (size_t i = 0; i < lines.size(); ++i) {
                if (i > 0) final_code += "\n";
                final_code += lines[i];
            }
            final_code += "\n";
        }

        std::string preamble =
            "import sys as __naab_sys, io as __naab_io, json as __naab_json\n"
            "__naab_buf = __naab_io.StringIO()\n"
            "__naab_orig = __naab_sys.stdout\n"
            "__naab_sys.stdout = __naab_buf\n";
        std::string postamble =
            "\n__naab_sys.stdout = __naab_orig\n"
            "__naab_captured = __naab_buf.getvalue().strip().split('\\n')\n"
            "__naab_result = None\n"
            "for __naab_l in reversed(__naab_captured):\n"
            "    __naab_l = __naab_l.strip()\n"
            "    if not __naab_l:\n"
            "        continue\n"
            "    try:\n"
            "        __naab_result = __naab_json.loads(__naab_l)\n"
            "        break\n"
            "    except:\n"
            "        __naab_sys.stdout.write(__naab_l + '\\n')\n"
            "__naab_result\n";
        final_code = preamble + final_code + postamble;
    }

    // FIX 19: Pre-execution advisory — check if code references NAAb variables not in binding list
    // Rewritten to eliminate false positives from module names, function names, and common words
    {
        auto all_env_names = current_env_->getAllNames();
        int warn_count = 0;

        // Strip string literals from code to avoid matching names inside strings
        std::string code_stripped;
        {
            bool in_sq = false, in_dq = false, in_bt = false, esc = false;
            for (size_t ci = 0; ci < code.size(); ++ci) {
                char c = code[ci];
                if (esc) { esc = false; continue; }
                if (c == '\\' && (in_sq || in_dq)) { esc = true; continue; }
                if (c == '"' && !in_sq && !in_bt) { in_dq = !in_dq; continue; }
                if (c == '\'' && !in_dq && !in_bt) { in_sq = !in_sq; continue; }
                if (c == '`' && !in_dq && !in_sq) { in_bt = !in_bt; continue; }
                if (!in_sq && !in_dq && !in_bt) code_stripped += c;
            }
        }

        // Common names that appear in every programming language — skip these
        static const std::unordered_set<std::string> SKIP_NAMES = {
            // Common programming identifiers
            "result", "data", "value", "error", "output", "input", "status",
            "count", "index", "size", "type", "name", "path", "file", "line",
            "code", "text", "list", "map", "set", "key", "var", "arg", "cmd",
            "buf", "len", "str", "num", "msg", "log", "tmp", "dir", "src",
            "dst", "ret", "err", "res", "req", "url", "pid", "bin", "env",
            "config", "options", "params", "args", "info", "mode", "port",
            "host", "body", "head", "response", "request", "server", "client",
            "test", "temp", "flag", "done", "init", "item", "next", "prev",
            "start", "stop", "state", "event", "task", "user", "help",
            // NAAb stdlib module names (modules, not variables)
            "io", "file", "json", "string", "array", "math", "time", "http",
            "env", "debug", "csv", "regex", "crypto", "os",
            // Shell/Python/Go/Rust builtins
            "true", "false", "null", "nil", "none", "self", "this",
            "print", "echo", "read", "write", "open", "close", "exit",
            "main", "func", "class", "struct", "enum", "match", "case",
        };

        for (const auto& env_name : all_env_names) {
            if (env_name.size() < 4) continue;  // Skip short names
            if (SKIP_NAMES.count(env_name)) continue;
            if (std::find(bound_vars.begin(), bound_vars.end(), env_name) != bound_vars.end()) continue;

            // Skip modules and functions (not bindable variables)
            auto val = current_env_->get(env_name);
            if (val) {
                // Skip if it holds a stdlib module marker string
                if (std::holds_alternative<std::string>(val->data)) {
                    const auto& s = std::get<std::string>(val->data);
                    if (s.size() >= 18 && s.substr(0, 18) == "__stdlib_module__:") continue;
                    if (s.size() >= 10 && s.substr(0, 10) == "__module__:") continue;
                }
                // Skip if it holds a function
                if (std::holds_alternative<std::shared_ptr<FunctionValue>>(val->data)) continue;
            }

            // Check if the variable name appears as a whole word in STRIPPED code
            size_t pos = code_stripped.find(env_name);
            while (pos != std::string::npos) {
                bool ws = (pos == 0 || (!std::isalnum(code_stripped[pos - 1]) && code_stripped[pos - 1] != '_'));
                bool we = (pos + env_name.size() >= code_stripped.size() ||
                           (!std::isalnum(code_stripped[pos + env_name.size()]) && code_stripped[pos + env_name.size()] != '_'));
                if (ws && we) {
                    fmt::print(stderr, "[WARN] Variable '{}' appears in <<{}>> block but is not in the binding list.\n"
                              "  If you need this NAAb variable, use: <<{}[{}{}]>>\n",
                              env_name, language, language,
                              (bound_vars.empty() ? "" : bound_vars[0] + ", "),
                              env_name);
                    warn_count++;
                    break;
                }
                pos = code_stripped.find(env_name, pos + 1);
            }
            if (warn_count >= 2) break;  // Max 2 warnings per block
        }

        // FIX-DX-5 (FIX 19b): Warn about bound variables not used in polyglot code
        for (const auto& bv : bound_vars) {
            bool found = false;
            size_t bpos = code_stripped.find(bv);
            while (bpos != std::string::npos) {
                bool ws = (bpos == 0 || (!std::isalnum(code_stripped[bpos - 1]) && code_stripped[bpos - 1] != '_'));
                bool we = (bpos + bv.size() >= code_stripped.size() ||
                           (!std::isalnum(code_stripped[bpos + bv.size()]) && code_stripped[bpos + bv.size()] != '_'));
                if (ws && we) { found = true; break; }
                bpos = code_stripped.find(bv, bpos + 1);
            }
            if (!found) {
                fmt::print(stderr, "[WARN] Bound variable '{}' is never used in <<{}>> block at {}:{}.\n",
                           bv, language, current_file_, node.getLocation().line);
            }
        }
    }

    // Layer 3: Polyglot-aware debug tracing (before execution)
    if (isDebugging() || verbose_mode_) {
        // Bug 8: Count lines from raw_code (not final_code which includes injected declarations)
        int line_count = static_cast<int>(std::count(raw_code.begin(), raw_code.end(), '\n')) + 1;
        fprintf(stderr, "\n[polyglot] -- %s block (%d lines) at %s:%d --\n",
                language.c_str(), line_count, current_file_.c_str(), node.getLocation().line);
        if (!bound_vars.empty()) {
            fprintf(stderr, "[polyglot] Bound variables:\n");
            for (const auto& var_name : bound_vars) {
                auto val = getVariable(var_name);
                if (val) fprintf(stderr, "  %s = %s\n", var_name.c_str(), val->toString().c_str());
            }
        }
        // Show first 5 lines of code
        std::istringstream code_preview(raw_code);
        std::string preview_line;
        int shown = 0;
        fprintf(stderr, "[polyglot] Code:\n");
        while (std::getline(code_preview, preview_line) && shown < 5) {
            fprintf(stderr, "  %d| %s\n", shown + 1, preview_line.c_str());
            shown++;
        }
        if (shown < line_count) fprintf(stderr, "  ... (%d more lines)\n", line_count - shown);
    }
    [[maybe_unused]] auto polyglot_trace_start = std::chrono::steady_clock::now();

    explain("Executing inline " + language + " code" +
            (bound_vars.empty() ? "" : " with " + std::to_string(bound_vars.size()) + " bound variables"));

    // Enterprise Security: Activate sandbox for polyglot execution
    auto& sandbox_manager = security::SandboxManager::instance();
    security::SandboxConfig sandbox_config = sandbox_manager.getDefaultConfig();

    // Governance: Override timeout if governance specifies one
    if (governance_ && governance_->isActive() && governance_->getTimeoutSeconds() > 0) {
        sandbox_config.max_cpu_seconds = governance_->getTimeoutSeconds();
    }

    security::ScopedSandbox scoped_sandbox(sandbox_config);

    // Phase 12: Create source mapper for error translation
    int var_decl_lines = static_cast<int>(std::count(var_declarations.begin(), var_declarations.end(), '\n'));
    runtime::SourceMapper source_mapper(current_file_, node.getLocation().line, node.getLocation().column);
    source_mapper.setOffset(var_decl_lines);

    // Phase 2.3: Execute the code and capture return value
    // Suspend GC during polyglot execution to prevent collecting live values
    gc_suspended_ = true;

    // Phase 1 Profiling: Start timing
    bool should_profile = governance_ && governance_->isProfilingEnabled();
    auto profile_start = should_profile ?
        std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    try {
        result_ = executor->executeWithReturn(final_code);

        // Polyglot Consensus Verification: cross-language result checking
        if (governance_ && governance_->isVerificationEnabled() && result_) {
            std::string result_str = result_->toString();
            std::string verify_err = governance_->verifyPolyglotResult(
                language, raw_code, result_str, node.getLocation().line);
            if (!verify_err.empty()) {
                gc_suspended_ = false;
                throw std::runtime_error(verify_err);
            }
        }

        // Output Baselines: check/record baseline for polyglot result
        if (governance_ && governance_->isActive() && result_ &&
            governance_->getRules().baselines.enabled) {
            std::string result_str_bl = result_->toString();
            std::string result_type_bl = getTypeName(result_);
            std::size_t code_hash = std::hash<std::string>{}(raw_code);
            char hash_buf_bl[16];
            snprintf(hash_buf_bl, sizeof(hash_buf_bl), "%06zx", code_hash & 0xFFFFFF);
            std::string baseline_key = language + ":" +
                (governance_->getRules().baselines.hash_keys ? std::string(hash_buf_bl) : "") +
                ":" + std::to_string(node.getLocation().line);
            std::string baseline_err = governance_->checkBaseline(
                baseline_key, result_str_bl, result_type_bl, node.getLocation().line);
            if (!baseline_err.empty()) {
                gc_suspended_ = false;
                throw std::runtime_error(baseline_err);
            }
        }

        // Phase 1 Profiling: Record timing on successful execution
        if (should_profile) {
            auto profile_end = std::chrono::steady_clock::now();
            auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
                profile_end - profile_start).count();

            // Compute simple code hash for dedup
            std::size_t hash_val = std::hash<std::string>{}(raw_code);
            char hash_buf[16];
            snprintf(hash_buf, sizeof(hash_buf), "%06zx", hash_val & 0xFFFFFF);

            // Task category: use "unknown" here, governance already classified it
            // during checkPolyglotBlock above
            std::string task_cat = "general";

            governance_->writeProfileEntry(language, task_cat, hash_buf, duration_us);
        }

        // ShellResult transparent handling: extract stdout or throw on failure
        if (result_ && std::holds_alternative<std::shared_ptr<StructValue>>(result_->data)) {
            auto& struct_val = std::get<std::shared_ptr<StructValue>>(result_->data);
            if (struct_val->type_name == "ShellResult" && struct_val->field_values.size() >= 3) {
                auto exit_code_val = struct_val->field_values[0];
                auto stdout_val = struct_val->field_values[1];
                auto stderr_val = struct_val->field_values[2];
                int exit_code = std::holds_alternative<int>(exit_code_val->data) ?
                    std::get<int>(exit_code_val->data) : -1;
                if (exit_code != 0) {
                    throw std::runtime_error(
                        "Shell command failed with exit code " + std::to_string(exit_code) + "\n"
                        "  stderr: " + stderr_val->toString() + "\n"
                        "  stdout: " + stdout_val->toString()
                    );
                }
                // Success: unwrap to just stdout value
                result_ = stdout_val;
            }
        }

        // Phase 12: Check for sentinel/JSON return values
        // Strategy 1: Check executor's captured output buffer (works for Python)
        std::string captured = executor->getCapturedOutput();
        bool sentinel_found = false;
        if (!captured.empty()) {
            auto polyglot_result = runtime::parsePolyglotOutput(captured, return_type);
            if (polyglot_result.return_value) {
                result_ = polyglot_result.return_value;
                sentinel_found = true;
            }
            // Print remaining log output
            if (!polyglot_result.log_output.empty()) {
                std::cout << polyglot_result.log_output << std::flush;
            }
        } else {
            // Flush stdout from executor (no captured output to parse)
            flushExecutorOutput(executor);
        }

        // Strategy 2: Check if result_ is a string containing the sentinel
        // (works for shell executor which returns stdout as string value)
        if (!sentinel_found && result_) {
            if (auto* str_val = std::get_if<std::string>(&result_->data)) {
                if (str_val->find("__NAAB_RETURN__:") != std::string::npos) {
                    auto polyglot_result = runtime::parsePolyglotOutput(*str_val, return_type);
                    if (polyglot_result.return_value) {
                        result_ = polyglot_result.return_value;
                    }
                    // Print log output that was mixed with the sentinel
                    if (!polyglot_result.log_output.empty()) {
                        std::cout << polyglot_result.log_output << std::flush;
                    }
                } else if (!return_type.empty()) {
                    // Strategy 3: If -> JSON header specified, try parsing result as JSON
                    auto polyglot_result = runtime::parsePolyglotOutput(*str_val, return_type);
                    if (polyglot_result.return_value) {
                        result_ = polyglot_result.return_value;
                    }
                }
            }
        }

        // Phase 12: BLOCK_CONTRACT_VIOLATION — -> JSON declared but no JSON produced
        if (!return_type.empty() && return_type == "JSON") {
            bool has_valid_result = result_ && !std::holds_alternative<std::monostate>(result_->data);
            if (!has_valid_result) {
                std::ostringstream oss;
                oss << "Block contract violation: <<" << language << " -> JSON>> expected a JSON return value, "
                    << "but no valid JSON was found in stdout.\n\n"
                    << "  The '-> JSON' header means the block MUST output valid JSON.\n"
                    << "  The last printed line must be a JSON string — not a bare value.\n\n"
                    << "  Common mistakes:\n"
                    << "  - Printing a bare number/string without JSON formatting\n"
                    << "  - Using 'return' instead of 'print' (polyglot uses stdout, not return)\n"
                    << "  - Printing debug output after the JSON line\n\n";
                if (language == "python") {
                    oss << "  \xE2\x9C\x97 Wrong:\n"
                        << "    <<python -> JSON\n"
                        << "    result = {\"key\": [1, 2, 3]}\n"
                        << "    result    # bare expression — NOT printed to stdout\n"
                        << "    >>\n\n"
                        << "  \xE2\x9C\x93 Right:\n"
                        << "    <<python -> JSON\n"
                        << "    import json\n"
                        << "    result = {\"key\": [1, 2, 3]}\n"
                        << "    print(json.dumps(result))    # explicit JSON output\n"
                        << "    >>\n";
                } else if (language == "javascript" || language == "js") {
                    oss << "  \xE2\x9C\x97 Wrong:\n"
                        << "    <<javascript -> JSON\n"
                        << "    const data = {key: [1, 2, 3]};\n"
                        << "    data;    // bare expression — NOT printed\n"
                        << "    >>\n\n"
                        << "  \xE2\x9C\x93 Right:\n"
                        << "    <<javascript -> JSON\n"
                        << "    const data = {key: [1, 2, 3]};\n"
                        << "    console.log(JSON.stringify(data));    // explicit JSON output\n"
                        << "    >>\n";
                } else if (language == "shell" || language == "bash" || language == "sh") {
                    oss << "  \xE2\x9C\x93 Right:\n"
                        << "    <<shell -> JSON\n"
                        << "    echo '{\"key\": [1, 2, 3]}'\n"
                        << "    >>\n";
                } else if (language == "go") {
                    oss << "  \xE2\x9C\x93 Right:\n"
                        << "    <<go -> JSON\n"
                        << "    package main\n"
                        << "    import (\"encoding/json\"; \"fmt\")\n"
                        << "    func main() {\n"
                        << "        data := map[string]interface{}{\"key\": []int{1, 2, 3}}\n"
                        << "        b, _ := json.Marshal(data)\n"
                        << "        fmt.Println(string(b))\n"
                        << "    }\n"
                        << "    >>\n";
                // FIX-DX-9: Nim-specific JSON error guidance
                } else if (language == "nim") {
                    oss << "  \xE2\x9C\x93 Right:\n"
                        << "    <<nim -> JSON\n"
                        << "    import json\n"
                        << "    echo $(%*{\"key\": %*[1, 2, 3]})\n"
                        << "    >>\n";
                } else {
                    oss << "  \xE2\x9C\x93 Right:\n"
                        << "    <<" << language << " -> JSON\n"
                        << "    // Print valid JSON as the LAST line of stdout\n"
                        << "    // e.g.: {\"key\": \"value\", \"count\": 42}\n"
                        << "    >>\n";
                }
                oss << "\n  Key rule: -> JSON requires the last stdout line to be valid JSON.\n";
                gc_suspended_ = false;
                throw std::runtime_error(oss.str());
            }

            // AMBIGUOUS_OUTPUT warning: result looks like an error, not structured data
            if (auto* str_val = std::get_if<std::string>(&result_->data)) {
                if (str_val->find("Traceback") != std::string::npos ||
                    str_val->find("Error") != std::string::npos ||
                    str_val->find("error:") != std::string::npos) {
                    std::cerr << "Warning: <<" << language << " -> JSON>> returned a string that looks "
                              << "like an error message, not JSON data. Consider using try/catch "
                              << "inside the polyglot block.\n";
                }
            }
        }

        gc_suspended_ = false;

        // Layer 3: Polyglot-aware debug tracing (after execution)
        if (isDebugging() || verbose_mode_) {
            auto polyglot_trace_end = std::chrono::steady_clock::now();
            double trace_ms = std::chrono::duration<double, std::milli>(
                polyglot_trace_end - polyglot_trace_start).count();
            std::string result_str = result_ ? result_->toString() : "null";
            if (result_str.size() > 200) result_str = result_str.substr(0, 200) + "...";
            fprintf(stderr, "[polyglot] %s returned: %s (%.1fms)\n",
                    language.c_str(), result_str.c_str(), trace_ms);
            fprintf(stderr, "[polyglot] -- end %s --\n\n", language.c_str());
        }

        // Governance v4: Log polyglot execution to audit trail
        if (governance_ && governance_->isActive()) {
            auto audit_end = std::chrono::steady_clock::now();
            int64_t duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
                audit_end - polyglot_trace_start).count();
            governance_->logPolyglotExecution(language, bound_vars, duration_us,
                                              current_file_, node.getLocation().line);
        }

    } catch (const std::exception& e) {
        gc_suspended_ = false;
        std::string error_msg = e.what();

        // Phase 12: Translate temp file paths to NAAb source locations
        std::string translated = source_mapper.translateError(error_msg);
        if (!translated.empty() && translated != error_msg) {
            // Prepend the NAAb source context to the error
            error_msg = translated + "\n  Original error: " + error_msg;
        }

        // Detect undefined variable errors and provide helpful guidance
        bool is_undefined_var = false;
        std::string var_name;

        // Python: "NameError: name 'x' is not defined"
        if (error_msg.find("NameError") != std::string::npos &&
            error_msg.find("not defined") != std::string::npos) {
            is_undefined_var = true;
            // Try to extract variable name between quotes
            size_t quote1 = error_msg.find('\'');
            if (quote1 != std::string::npos) {
                size_t quote2 = error_msg.find('\'', quote1 + 1);
                if (quote2 != std::string::npos) {
                    var_name = error_msg.substr(quote1 + 1, quote2 - quote1 - 1);
                }
            }
        }

        // JavaScript: "ReferenceError: x is not defined"
        if (!is_undefined_var && error_msg.find("ReferenceError") != std::string::npos &&
            error_msg.find("is not defined") != std::string::npos) {
            is_undefined_var = true;
            // Extract variable name before "is not defined"
            size_t pos = error_msg.find("is not defined");
            if (pos != std::string::npos) {
                // Look backwards for the variable name
                std::string prefix = error_msg.substr(0, pos);
                size_t last_space = prefix.find_last_of(" :");
                if (last_space != std::string::npos) {
                    var_name = prefix.substr(last_space + 1);
                    // Trim whitespace
                    var_name.erase(0, var_name.find_first_not_of(" \t"));
                    var_name.erase(var_name.find_last_not_of(" \t") + 1);
                }
            }
        }

        // Python fallback: "name 'x' is not defined" (without NameError: prefix)
        if (!is_undefined_var && error_msg.find("name '") != std::string::npos &&
            error_msg.find("' is not defined") != std::string::npos) {
            is_undefined_var = true;
            size_t q1 = error_msg.find("name '") + 6;
            size_t q2 = error_msg.find("'", q1);
            if (q2 != std::string::npos) var_name = error_msg.substr(q1, q2 - q1);
        }

        // Go: "undefined: x"
        if (!is_undefined_var && error_msg.find("undefined: ") != std::string::npos) {
            is_undefined_var = true;
            size_t pos = error_msg.find("undefined: ");
            var_name = error_msg.substr(pos + 11);
            size_t end = var_name.find_first_of(" \t\n\r");
            if (end != std::string::npos) var_name = var_name.substr(0, end);
        }

        // Ruby: "undefined local variable or method 'x'"
        if (!is_undefined_var && error_msg.find("undefined local variable") != std::string::npos) {
            is_undefined_var = true;
            size_t q1 = error_msg.find("'");
            if (q1 != std::string::npos) {
                size_t q2 = error_msg.find("'", q1 + 1);
                if (q2 != std::string::npos) var_name = error_msg.substr(q1 + 1, q2 - q1 - 1);
            }
        }

        // Nim: "undeclared identifier: 'x'"
        if (!is_undefined_var && error_msg.find("undeclared identifier") != std::string::npos) {
            is_undefined_var = true;
            size_t q1 = error_msg.find("'");
            if (q1 != std::string::npos) {
                size_t q2 = error_msg.find("'", q1 + 1);
                if (q2 != std::string::npos) var_name = error_msg.substr(q1 + 1, q2 - q1 - 1);
            }
        }

        if (is_undefined_var) {
            std::ostringstream oss;
            oss << "Inline " << language << " execution failed: " << error_msg << "\n\n";
            oss << "  Help: Did you forget to bind a NAAb variable?\n";
            oss << "  Inline polyglot code requires explicit variable binding syntax.\n\n";

            if (!var_name.empty()) {
                oss << "  ✗ Wrong - variable not bound:\n";
                oss << "    let result = <<" << language << "\n";
                oss << "    " << var_name << " * 2\n";
                oss << "    >>\n\n";
                oss << "  ✓ Right - explicit variable binding:\n";
                oss << "    let result = <<" << language << "[" << var_name << "]\n";
                oss << "    " << var_name << " * 2\n";
                oss << "    >>\n\n";
            } else {
                oss << "  Syntax: <<language[var1, var2, ...]\n";
                oss << "    your code here\n";
                oss << "  >>\n\n";
            }

            oss << "  Example with multiple variables:\n";
            oss << "    let a = 10\n";
            oss << "    let b = 20\n";
            oss << "    let sum = <<" << language << "[a, b]\n";
            oss << "    a + b\n";
            oss << "    >>\n";

            throw std::runtime_error(oss.str());
        }

        // Detect common polyglot errors and add helpful context
        std::ostringstream oss;
        oss << "Inline " << language << " execution failed: " << error_msg << "\n";

        // Python indentation errors
        if (error_msg.find("IndentationError") != std::string::npos ||
            error_msg.find("unexpected indent") != std::string::npos) {
            oss << "\n  Help: Python indentation error in polyglot block.\n"
                << "  Common causes:\n"
                << "  - Mixing tabs and spaces\n"
                << "  - Code inside the block has inconsistent indentation\n"
                << "  - All lines in the block should use the same indentation style\n\n"
                << "  ✗ Wrong - inconsistent indentation:\n"
                << "    let r = <<python\n"
                << "    x = 1\n"
                << "      y = 2   # extra indent!\n"
                << "    >>\n\n"
                << "  ✓ Right - consistent indentation:\n"
                << "    let r = <<python\n"
                << "    x = 1\n"
                << "    y = 2\n"
                << "    >>\n";
        }
        // Python 'return' outside function
        else if (language == "python" &&
                 error_msg.find("return") != std::string::npos &&
                 error_msg.find("outside function") != std::string::npos &&
                 raw_code.find("return") != std::string::npos) {
            oss << "\n  Help: Do NOT use 'return' in Python polyglot blocks.\n"
                << "  Python polyglot blocks are NOT inside a function.\n"
                << "  The last expression's value is automatically returned to NAAb.\n\n"
                << "  ✗ Wrong:\n"
                << "    let r = <<python\n"
                << "    return json.dumps(data)\n"
                << "    >>\n\n"
                << "  ✓ Right:\n"
                << "    let r = <<python\n"
                << "    json.dumps(data)\n"
                << "    >>\n";
        }
        // Python SyntaxError
        else if (language == "python" && error_msg.find("SyntaxError") != std::string::npos) {
            oss << "\n  Help: Python syntax error in polyglot block.\n"
                << "  Common causes:\n"
                << "  - Missing colons after if/for/def/class\n"
                << "  - Unclosed parentheses or brackets\n"
                << "  - Python 3 syntax required (print is a function)\n\n"
                << "  Tip: The last expression in the block is the return value.\n"
                << "  For multi-line blocks, put the result on the last line:\n"
                << "    let r = <<python\n"
                << "    x = compute()\n"
                << "    x  # this value is returned to NAAb\n"
                << "    >>\n";
        }
        // Python/JS import errors
        else if (error_msg.find("ModuleNotFoundError") != std::string::npos ||
                 error_msg.find("ImportError") != std::string::npos ||
                 error_msg.find("Cannot find module") != std::string::npos) {
            oss << "\n  Help: Missing module/package in " << language << " polyglot block.\n"
                << "  The module needs to be installed in your system's " << language << " environment.\n\n"
                << "  For Python: pip install <module_name>\n"
                << "  For JavaScript: npm install <module_name>\n\n"
                << "  Note: Only standard library modules are available by default.\n";
        }
        // Compilation errors (Rust, C++, C#, Go, Nim, Zig)
        else if (error_msg.find("compilation failed") != std::string::npos) {
            // Map language to compiler name
            std::string compiler = "g++";
            if (language == "rust") compiler = "rustc";
            else if (language == "csharp" || language == "cs") compiler = "mcs";
            else if (language == "go") compiler = "go build";
            else if (language == "nim") compiler = "nim c";
            else if (language == "zig") compiler = "zig build-exe";
            else if (language == "julia") compiler = "julia";

            oss << "\n  Help: Compilation error in " << language << " polyglot block.\n"
                << "  The " << language << " compiler rejected the generated code.\n"
                << "  Check that the code is valid " << language << " and that\n"
                << "  the compiler (" << compiler << ") is installed.\n\n"
                << "  Tip: NAAb wraps single expressions automatically.\n"
                << "  For multi-statement blocks, write a complete program.\n";
        }
        // JavaScript: 'return' keyword in expression context
        else if (language == "javascript" &&
                 (error_msg.find("unexpected token") != std::string::npos ||
                  error_msg.find("SyntaxError") != std::string::npos) &&
                 error_msg.find("return") != std::string::npos) {
            oss << "\n  Help: Don't use 'return' in JavaScript polyglot blocks.\n"
                << "  The last expression is automatically returned to NAAb.\n\n"
                << "  ✗ Wrong:\n"
                << "    let x = <<javascript\n"
                << "    return 42\n"
                << "    >>\n\n"
                << "  ✓ Right:\n"
                << "    let x = <<javascript\n"
                << "    42\n"
                << "    >>\n\n"
                << "  For multi-line blocks:\n"
                << "    let x = <<javascript\n"
                << "    let result = someComputation();\n"
                << "    result   // last expression is the return value\n"
                << "    >>\n";
        }
        // TypeScript syntax errors (tsx/tsc)
        else if ((language == "typescript" || language == "ts") &&
                 (error_msg.find("Expected") != std::string::npos ||
                  error_msg.find("SyntaxError") != std::string::npos ||
                  error_msg.find("error TS") != std::string::npos ||
                  error_msg.find("Cannot find") != std::string::npos)) {
            oss << "\n  Help: TypeScript syntax error in polyglot block.\n"
                << "  NAAb injects bound variables as `const name = value;` before your code\n"
                << "  and wraps the last expression in console.log() for return capture.\n\n"
                << "  Common causes:\n"
                << "  - Braces/blocks confuse the auto-wrapping (use explicit console.log)\n"
                << "  - Variable injection collides with import statements\n"
                << "  - Type annotations on injected values (NAAb injects `const`, not typed)\n\n"
                << "  ✗ Fragile — auto-wrapping may break with blocks:\n"
                << "    let r = <<typescript[x]\n"
                << "    if (x > 0) { \"positive\" } else { \"negative\" }\n"
                << "    >>\n\n"
                << "  ✓ Robust — explicit console.log:\n"
                << "    let r = <<typescript[x]\n"
                << "    const result = x > 0 ? \"positive\" : \"negative\";\n"
                << "    console.log(result);\n"
                << "    >>\n\n"
                << "  ✓ Best — use naab_return() for structured data:\n"
                << "    let r = <<typescript[x]\n"
                << "    naab_return({value: x, label: \"result\"});\n"
                << "    >>\n\n"
                << "  Tip: Put imports FIRST in the block (before any logic).\n"
                << "  NAAb injects variables after import lines automatically.\n";
        }
        // Go: package main collision with variable injection
        else if ((language == "go") &&
                 (error_msg.find("expected 'package'") != std::string::npos ||
                  error_msg.find("expected package") != std::string::npos)) {
            oss << "\n  Help: Go requires 'package main' as the first line.\n"
                << "  NAAb injects bound variables after package/import headers,\n"
                << "  but if the block structure is unusual, injection can collide.\n\n"
                << "  ✓ Correct — package main first, then imports:\n"
                << "    let r = <<go[x]\n"
                << "    package main\n"
                << "    import \"fmt\"\n"
                << "    func main() {\n"
                << "        fmt.Println(x)\n"
                << "    }\n"
                << "    >>\n\n"
                << "  ✓ Simple — let NAAb auto-wrap (no package main needed):\n"
                << "    let r = <<go[x]\n"
                << "    x * 2\n"
                << "    >>\n\n"
                << "  Tip: For simple expressions, omit package main entirely.\n"
                << "  NAAb wraps Go expressions in package main automatically.\n";
        }
        // Rust: common injection issues
        else if ((language == "rust") &&
                 (error_msg.find("expected") != std::string::npos ||
                  error_msg.find("cannot find") != std::string::npos)) {
            oss << "\n  Help: Rust compilation error in polyglot block.\n"
                << "  NAAb injects bound variables as `let name = value;` before your code.\n"
                << "  For complex types (arrays, dicts), NAAb uses a JSON context file.\n\n"
                << "  Common causes:\n"
                << "  - Variable type mismatch (NAAb infers types from values)\n"
                << "  - Missing use/extern crate for libraries\n"
                << "  - Rust's strict type system rejecting injected values\n\n"
                << "  ✓ Simple expressions (auto-wrapped in fn main):\n"
                << "    let r = <<rust[x]\n"
                << "    x * 2\n"
                << "    >>\n\n"
                << "  ✓ Full programs:\n"
                << "    let r = <<rust[x]\n"
                << "    fn main() {\n"
                << "        println!(\"{}\", x * 2);\n"
                << "    }\n"
                << "    >>\n";
        }
        // Go: undefined: print → suggest fmt.Println
        else if (language == "go" && error_msg.find("undefined: print") != std::string::npos) {
            oss << "\n  Help: Go doesn't have a built-in print() function.\n"
                << "  Use fmt.Println() instead:\n\n"
                << "    \xE2\x9C\x97 Wrong: print(\"hello\")\n"
                << "    \xE2\x9C\x93 Right: fmt.Println(\"hello\")\n";
        }
        // Go: undefined: console → JS idiom
        else if (language == "go" && error_msg.find("undefined: console") != std::string::npos) {
            oss << "\n  Help: console.log() is JavaScript, not Go.\n"
                << "  Use fmt.Println() instead.\n";
        }
        // Go: type mismatch from variable injection
        else if (language == "go" && error_msg.find("cannot use") != std::string::npos &&
                 error_msg.find("as type") != std::string::npos) {
            oss << "\n  Help: Go type mismatch with bound variable.\n"
                << "  You may need a type assertion:\n\n"
                << "    val := boundVar.(int)  // assert concrete type\n"
                << "  Or use fmt.Sprint(boundVar) for string conversion.\n";
        }
        // Ruby: unexpected end-of-input → missing 'end'
        else if (language == "ruby" && error_msg.find("unexpected end-of-input") != std::string::npos) {
            oss << "\n  Help: Ruby requires 'end' to close blocks (def, if, do, class).\n"
                << "  Unlike Python, indentation doesn't define blocks in Ruby.\n\n"
                << "    \xE2\x9C\x97 Wrong (Python-style):\n"
                << "      def greet(name)\n"
                << "        puts \"Hello #{name}\"\n\n"
                << "    \xE2\x9C\x93 Right (Ruby-style):\n"
                << "      def greet(name)\n"
                << "        puts \"Hello #{name}\"\n"
                << "      end\n";
        }
        // Nim: undeclared identifier: 'print' → use echo
        else if (language == "nim" && error_msg.find("undeclared identifier") != std::string::npos &&
                 error_msg.find("print") != std::string::npos) {
            oss << "\n  Help: Nim uses 'echo' for output, not 'print()':\n"
                << "    \xE2\x9C\x97 Wrong: print(\"hello\")\n"
                << "    \xE2\x9C\x93 Right: echo \"hello\"\n";
        }
        // Shell: print not found → use echo
        else if ((language == "shell" || language == "bash" || language == "sh") &&
                 error_msg.find("not found") != std::string::npos &&
                 error_msg.find("print") != std::string::npos) {
            oss << "\n  Help: Shell uses 'echo' for output, not 'print()':\n"
                << "    \xE2\x9C\x97 Wrong: print(\"hello\")\n"
                << "    \xE2\x9C\x93 Right: echo \"hello\"\n";
        }
        // C#: compilation errors
        else if ((language == "csharp" || language == "cs") &&
                 (error_msg.find("error CS") != std::string::npos ||
                  error_msg.find("compilation failed") != std::string::npos)) {
            oss << "\n  Help: C# compilation error in polyglot block.\n"
                << "  NAAb injects bound variables as `var name = value;` before your code.\n\n"
                << "  Common causes:\n"
                << "  - Missing using directives (add at top of block)\n"
                << "  - NAAb uses Mono's mcs compiler - ensure compatibility\n"
                << "  - For return values, use Console.Write() (not Console.WriteLine)\n";
        }
        // Nim: compilation errors
        else if (language == "nim" &&
                 error_msg.find("Error:") != std::string::npos) {
            oss << "\n  Help: Nim compilation error in polyglot block.\n"
                << "  Nim is indentation-sensitive (like Python).\n\n"
                << "  Common causes:\n"
                << "  - Inconsistent indentation (use spaces, not tabs)\n"
                << "  - Missing imports (import strutils, json, etc.)\n"
                << "  - Use 'echo' for output, not 'print'\n";
        }
        // Julia: errors
        else if (language == "julia" &&
                 error_msg.find("ERROR:") != std::string::npos) {
            oss << "\n  Help: Julia error in polyglot block.\n\n"
                << "  Common causes:\n"
                << "  - Missing 'using' for packages\n"
                << "  - Julia uses 'println()' for output\n"
                << "  - Julia uses 'nothing' instead of null/None\n"
                << "  - Arrays are 1-indexed in Julia\n";
        }
        // Ruby: common errors
        else if (language == "ruby" &&
                 (error_msg.find("SyntaxError") != std::string::npos ||
                  error_msg.find("NameError") != std::string::npos ||
                  error_msg.find("NoMethodError") != std::string::npos)) {
            oss << "\n  Help: Ruby error in polyglot block.\n\n"
                << "  Common causes:\n"
                << "  - Missing 'end' for blocks (def, if, do, class)\n"
                << "  - Use 'puts' for output (not print() like Python)\n"
                << "  - Ruby uses 'nil' instead of null/None\n"
                << "  - String interpolation: \"Hello #{name}\" (not f-strings)\n";
        }
        // PHP: errors
        else if (language == "php" &&
                 (error_msg.find("Parse error") != std::string::npos ||
                  error_msg.find("Fatal error") != std::string::npos)) {
            oss << "\n  Help: PHP error in polyglot block.\n"
                << "  NAAb adds <?php automatically for variable injection.\n\n"
                << "  Common causes:\n"
                << "  - Don't add <?php if NAAb already injected variables\n"
                << "  - Variables use $name syntax in PHP\n"
                << "  - Use echo for output\n"
                << "  - Statements need semicolons\n";
        }
        // Zig: errors
        else if (language == "zig" &&
                 error_msg.find("error:") != std::string::npos) {
            oss << "\n  Help: Zig compilation error in polyglot block.\n\n"
                << "  Common causes:\n"
                << "  - All errors must be handled (try/catch or |_| syntax)\n"
                << "  - Use std.debug.print() for output\n"
                << "  - Zig uses 'null' for optional values\n"
                << "  - Type annotations are often required\n";
        }
        // Generic: Python None return causing null
        else if (error_msg.find("Cannot infer type") != std::string::npos &&
                 error_msg.find("null") != std::string::npos) {
            oss << "\n  Help: Polyglot block returned null (Python None).\n"
                << "  Make sure the last expression in the block has a value:\n\n"
                << "  ✗ Wrong - print() returns None:\n"
                << "    let x = <<python\n"
                << "    print('hello')\n"
                << "    >>\n\n"
                << "  ✓ Right - last expression has a value:\n"
                << "    let x = <<python\n"
                << "    result = 'hello'\n"
                << "    result\n"
                << "    >>\n";
        }

        // Layer 3: Enhanced error context in debug mode
        if (isDebugging()) {
            oss << "\n  [debug] Full code sent to " << language << " executor:\n";
            std::istringstream dbg_code_stream(final_code);
            std::string dbg_code_line;
            int dbg_line_num = 1;
            while (std::getline(dbg_code_stream, dbg_code_line)) {
                oss << "    " << dbg_line_num++ << "| " << dbg_code_line << "\n";
            }
            if (!bound_vars.empty()) {
                oss << "\n  [debug] Bound variables at time of error:\n";
                for (const auto& bv : bound_vars) {
                    auto val = getVariable(bv);
                    if (val) oss << "    " << bv << " = " << val->toString() << "\n";
                }
            } else {
                oss << "\n  [debug] No variables were bound to this block.\n";
            }
        }

        throw std::runtime_error(oss.str());
    }
}

// ============================================================================
// StructValue Methods

void Interpreter::VariableSnapshot::capture(
    Environment* env,
    const std::vector<std::string>& var_names,
    Interpreter* interp
) {
    for (const auto& name : var_names) {
        if (env->has(name)) {
            // Deep copy the value to avoid shared mutable state
            auto original_value = env->get(name);
            auto copied_value = interp->copyValue(original_value);
            variables[name] = copied_value;
        }
    }
}

// Parallel polyglot execution: Execute a group of polyglot blocks in parallel
void Interpreter::executePolyglotGroupParallel(const DependencyGroup& group) {

    if (group.parallel_blocks.empty()) {
        return;  // Nothing to execute
    }

    // Enterprise Security: Activate sandbox for parallel polyglot execution
    auto& sandbox_manager = security::SandboxManager::instance();

    security::SandboxConfig sandbox_config = sandbox_manager.getDefaultConfig();

    security::ScopedSandbox scoped_sandbox(sandbox_config);


    // Always use parallel execution, even for single blocks
    // This avoids Python segfault in sequential path and ensures consistency
    // Step 1: Capture variable snapshots for each block (thread-safe deep copy)
    std::vector<VariableSnapshot> snapshots;
    for (size_t block_idx = 0; block_idx < group.parallel_blocks.size(); ++block_idx) {
        const auto& block = group.parallel_blocks[block_idx];

        VariableSnapshot snapshot;
        snapshot.capture(current_env_.get(), block.read_vars, this);

        snapshots.push_back(std::move(snapshot));
    }

    // Step 2: Prepare code for each block with variable bindings
    std::vector<std::tuple<
        polyglot::PolyglotAsyncExecutor::Language,
        std::string,
        std::vector<interpreter::Value>
    >> tasks;

    // Track which blocks from group.parallel_blocks were submitted for parallel execution
    // Sequential blocks (lang_supported=false) are executed inline and skipped from tasks
    std::vector<size_t> parallel_block_indices;
    // Track return types for JSON Sovereign Pipe handling in result processing
    std::vector<std::string> parallel_return_types;

    for (size_t i = 0; i < group.parallel_blocks.size(); ++i) {
        const auto& block = group.parallel_blocks[i];
        const auto& snapshot = snapshots[i];
        auto* inline_code = block.node;

        // Convert language string to enum
        std::string lang_str = inline_code->getLanguage();
        polyglot::PolyglotAsyncExecutor::Language lang;

        // Check if language is supported by PolyglotAsyncExecutor
        bool lang_supported = true;

        if (lang_str == "python") {
            // Execute Python sequentially on main thread to avoid fragmenting
            // address space with CFI shadow entries in worker threads.
            // Python thread pool execution creates new CFI mappings that make
            // subsequent fork/posix_spawn calls fail with SIGABRT on Android.
            // The main thread uses PyGILState_Ensure which is safe.
            lang_supported = false;
            lang = polyglot::PolyglotAsyncExecutor::Language::Python;
        } else if (lang_str == "javascript" || lang_str == "js") {
            // Thread pool allows safe parallel execution
            lang = polyglot::PolyglotAsyncExecutor::Language::JavaScript;
        } else if (lang_str == "cpp" || lang_str == "c++") {
            // C++ uses fork/exec for compilation - sequential to avoid CFI crash
            // fork/exec already creates parallel subprocesses
            lang_supported = false;
            lang = polyglot::PolyglotAsyncExecutor::Language::Cpp;
        } else if (lang_str == "rust") {
            lang_supported = false;
            lang = polyglot::PolyglotAsyncExecutor::Language::Rust;
        } else if (lang_str == "csharp" || lang_str == "cs") {
            lang_supported = false;
            lang = polyglot::PolyglotAsyncExecutor::Language::CSharp;
        } else if (lang_str == "shell" || lang_str == "bash" || lang_str == "sh") {
            // Shell uses fork/exec which triggers Android bionic CFI crash
            // from thread pool workers. No need for thread pool anyway -
            // fork/exec already creates a parallel subprocess.
            lang_supported = false;
            lang = polyglot::PolyglotAsyncExecutor::Language::Shell;
        } else {
            // Unsupported language for parallel execution (e.g., go, ruby, perl)
            // Fall back to sequential execution using LanguageRegistry
            lang_supported = false;
            lang = polyglot::PolyglotAsyncExecutor::Language::GenericSubprocess;  // Placeholder
        }

        // If language not supported for parallel execution, execute sequentially
        if (!lang_supported) {
            // Execute the full statement sequentially (e.g., VarDeclStmt)
            // This ensures the variable gets properly assigned
            auto* stmt = block.statement;
            stmt->accept(*this);

            // Skip adding to parallel tasks
            continue;
        }

        // Prepare variable declarations by serializing snapshot values
        std::string var_declarations;
        for (const auto& [var_name, value] : snapshot.variables) {
            std::string serialized = serializeValueForLanguage(value, lang_str);

            // Language-specific variable declaration syntax
            if (lang_str == "python") {
                var_declarations += var_name + " = " + serialized + "\n";
            } else if (lang_str == "javascript" || lang_str == "js") {
                var_declarations += "const " + var_name + " = " + serialized + ";\n";
            } else if (lang_str == "rust") {
                var_declarations += "let " + var_name + " = " + serialized + ";\n";
            } else if (lang_str == "cpp" || lang_str == "c++") {
                var_declarations += "const auto " + var_name + " = " + serialized + ";\n";
            } else if (lang_str == "csharp" || lang_str == "cs") {
                var_declarations += "var " + var_name + " = " + serialized + ";\n";
            } else if (lang_str == "shell" || lang_str == "bash") {
                var_declarations += var_name + "=" + serialized + "\n";
            } else {
                // Generic: assume C-like syntax
                var_declarations += var_name + " = " + serialized + ";\n";
            }
        }

        // Get raw code and strip common indentation
        std::string raw_code = inline_code->getCode();

        // Governance v3.0: Check polyglot block in parallel execution path
        if (governance_ && governance_->isActive()) {
            int gov_line = inline_code->getLocation().line;
            std::string gov_err = governance_->checkPolyglotBlock(
                lang_str, raw_code, current_file_, gov_line,
                inline_code->getBoundVariables().size());
            if (!gov_err.empty()) throw std::runtime_error(gov_err);

            std::string count_err = governance_->incrementAndCheckPolyglotBlockCount();
            if (!count_err.empty()) throw std::runtime_error(count_err);

            // BUG-J + BUG-4 + FIX-D: Taint sink check for ALL language bindings
            checkPolyglotBoundVarTaint(lang_str, inline_code->getBoundVariables(), gov_line);
        }

        std::vector<std::string> lines;
        std::istringstream stream(raw_code);
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(line);
        }

        // Find minimum indentation (ignoring empty lines)
        size_t min_indent = std::string::npos;
        for (const auto& l : lines) {
            if (l.empty() || l.find_first_not_of(" \t") == std::string::npos) continue;
            size_t indent = l.find_first_not_of(" \t");
            if (indent < min_indent) min_indent = indent;
        }

        // Strip the common indentation from all lines
        std::string code;
        for (const auto& l : lines) {
            if (l.empty() || l.find_first_not_of(" \t") == std::string::npos) {
                code += "\n";
            } else {
                if (min_indent != std::string::npos && l.length() > min_indent) {
                    code += l.substr(min_indent) + "\n";
                } else {
                    code += l + "\n";
                }
            }
        }

        // Phase 12: Get return type for JSON Sovereign Pipe support
        std::string return_type = inline_code->getReturnType();

        // Phase 12: Inject naab_return() helper for parallel execution path
        // Only inject if naab_return is actually used in the code (avoids breaking IIFE wrapping)
        if (raw_code.find("naab_return") != std::string::npos) {
            std::string helper;
            if (lang_str == "python") {
                if (!return_type.empty()) {
                    // Python with -> JSON: naab_return prints to stdout for StringIO capture
                    helper = "def naab_return(data):\n    import json as __nrj\n    print(__nrj.dumps(data) if not isinstance(data, str) else data)\n";
                } else {
                    helper = "def naab_return(data):\n    return data\n";
                }
            } else if (lang_str == "javascript" || lang_str == "js") {
                helper = "function naab_return(data) { return data; }\n";
            } else if (lang_str == "typescript" || lang_str == "ts") {
                helper = "function naab_return(data) { return data; }\n";
            } else if (lang_str == "ruby") {
                helper = "require 'json'\ndef naab_return(data); puts \"__NAAB_RETURN__:\" + data.to_json; end\n";
            } else if (lang_str == "php") {
                helper = "function naab_return($data) { echo \"__NAAB_RETURN__:\" . json_encode($data) . \"\\n\"; }\n";
            } else if (lang_str == "shell" || lang_str == "sh" || lang_str == "bash") {
                helper = "naab_return() { echo \"__NAAB_RETURN__:$1\"; }\n";
            } else if (lang_str == "rust") {
                helper = "macro_rules! naab_return { ($val:expr) => { println!(\"__NAAB_RETURN__:{}\", $val); }; }\n";
            } else if (lang_str == "cpp" || lang_str == "c++") {
                helper = "#include <sstream>\n#define naab_return(val) do { std::ostringstream __os; __os << \"__NAAB_RETURN__:\" << (val); std::cout << __os.str() << std::endl; } while(0)\n";
            }
            if (!helper.empty()) {
                var_declarations = helper + var_declarations;
            }
        }

        // Prepend variable declarations with header awareness
        std::string final_code;
        if (!var_declarations.empty() &&
            (lang_str == "go" || lang_str == "php" ||
             lang_str == "typescript" || lang_str == "ts")) {
            final_code = injectDeclarationsAfterHeaders(var_declarations, code, lang_str);
        } else {
            final_code = var_declarations + code;
        }

        // Phase 12: For Python with -> JSON, wrap code to capture stdout and extract last JSON line
        if (!return_type.empty() && (lang_str == "python")) {
            // Auto-wrap bare Python expressions in print() for -> JSON
            {
                std::istringstream iss(final_code);
                std::string line;
                std::vector<std::string> lines;
                while (std::getline(iss, line)) {
                    lines.push_back(line);
                }
                for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
                    std::string trimmed = lines[i];
                    size_t start = trimmed.find_first_not_of(" \t");
                    if (start == std::string::npos) continue;
                    trimmed = trimmed.substr(start);
                    if (trimmed[0] == '#') continue;
                    if (trimmed.substr(0, 7) == "import " || trimmed.substr(0, 5) == "from ") continue;
                    if (trimmed.substr(0, 6) == "print(" || trimmed.substr(0, 7) == "print (") break;
                    if (trimmed.find('=') != std::string::npos && trimmed.find("==") == std::string::npos
                        && trimmed.find("!=") == std::string::npos && trimmed.find(">=") == std::string::npos
                        && trimmed.find("<=") == std::string::npos) break;
                    if (trimmed.substr(0, 3) == "if " || trimmed.substr(0, 4) == "for "
                        || trimmed.substr(0, 6) == "while " || trimmed.substr(0, 4) == "def "
                        || trimmed.substr(0, 6) == "class " || trimmed.substr(0, 4) == "try:"
                        || trimmed.substr(0, 7) == "except:" || trimmed.substr(0, 7) == "except "
                        || trimmed.substr(0, 4) == "with ") break;
                    std::string leading = lines[i].substr(0, start);
                    lines[i] = leading + "print(" + trimmed + ")";
                    break;
                }
                final_code.clear();
                for (size_t i = 0; i < lines.size(); ++i) {
                    if (i > 0) final_code += "\n";
                    final_code += lines[i];
                }
                final_code += "\n";
            }

            std::string preamble =
                "import sys as __naab_sys, io as __naab_io, json as __naab_json\n"
                "__naab_buf = __naab_io.StringIO()\n"
                "__naab_orig = __naab_sys.stdout\n"
                "__naab_sys.stdout = __naab_buf\n";
            std::string postamble =
                "\n__naab_sys.stdout = __naab_orig\n"
                "__naab_captured = __naab_buf.getvalue().strip().split('\\n')\n"
                "__naab_result = None\n"
                "for __naab_l in reversed(__naab_captured):\n"
                "    __naab_l = __naab_l.strip()\n"
                "    if not __naab_l:\n"
                "        continue\n"
                "    try:\n"
                "        __naab_result = __naab_json.loads(__naab_l)\n"
                "        break\n"
                "    except:\n"
                "        __naab_sys.stdout.write(__naab_l + '\\n')\n"
                "__naab_result\n";
            final_code = preamble + final_code + postamble;
        }

        // Create task with empty args (variables are injected into code)
        std::vector<interpreter::Value> args;
        tasks.emplace_back(lang, final_code, args);
        parallel_block_indices.push_back(i);
        parallel_return_types.push_back(return_type);
    }

    // Step 3: Execute in parallel using PolyglotAsyncExecutor
    // Suspend GC during polyglot execution to prevent collecting live values
    gc_suspended_ = true;

    polyglot::PolyglotAsyncExecutor executor;
    int timeout_ms = 30000; // default
    if (governance_ && governance_->isActive() && governance_->getTimeoutSeconds() > 0) {
        timeout_ms = governance_->getTimeoutSeconds() * 1000;
    }

    auto results = [&]() {
        try {
            return executor.executeParallel(tasks, std::chrono::milliseconds(timeout_ms));
        } catch (...) {
            gc_suspended_ = false;
            throw;
        }
    }();

    // Step 4: Store results back to environment (sequential, thread-safe)
    // IMPORTANT: results[j] corresponds to parallel_block_indices[j], NOT group.parallel_blocks[j]
    // because sequential blocks were already executed and skipped from tasks
    for (size_t j = 0; j < results.size(); ++j) {
        size_t block_idx = parallel_block_indices[j];
        const auto& block = group.parallel_blocks[block_idx];
        const auto& result = results[j];

        std::string return_type = parallel_return_types[j];
        std::string lang_str = block.node ? block.node->getLanguage() : "unknown";

        if (result.success) {
            auto value = std::make_shared<Value>(result.value);

            // Phase 12: Check for sentinel/JSON return values in parallel path
            if (auto* str_val = std::get_if<std::string>(&value->data)) {
                if (str_val->find("__NAAB_RETURN__:") != std::string::npos) {
                    auto polyglot_result = runtime::parsePolyglotOutput(*str_val, return_type);
                    if (polyglot_result.return_value) {
                        value = polyglot_result.return_value;
                    }
                    if (!polyglot_result.log_output.empty()) {
                        std::cout << polyglot_result.log_output << std::flush;
                    }
                } else if (!return_type.empty()) {
                    // -> JSON header: try parsing result as JSON
                    auto polyglot_result = runtime::parsePolyglotOutput(*str_val, return_type);
                    if (polyglot_result.return_value) {
                        value = polyglot_result.return_value;
                    }
                }
            }

            // Phase 12: BLOCK_CONTRACT_VIOLATION — -> JSON declared but no JSON produced
            if (!return_type.empty() && return_type == "JSON") {
                bool has_valid_result = value && !std::holds_alternative<std::monostate>(value->data);
                if (!has_valid_result) {
                    std::ostringstream oss;
                    oss << "Block contract violation: <<" << lang_str << " -> JSON>> expected a JSON return value, "
                        << "but no valid JSON was found in stdout.\n\n"
                        << "  The '-> JSON' header means the block MUST output valid JSON.\n"
                        << "  The last printed line must be a JSON string.\n\n";
                    if (lang_str == "python") {
                        oss << "  \xE2\x9C\x93 Right:\n"
                            << "    <<python -> JSON\n"
                            << "    import json\n"
                            << "    result = {\"key\": [1, 2, 3]}\n"
                            << "    print(json.dumps(result))\n"
                            << "    >>\n";
                    } else if (lang_str == "javascript" || lang_str == "js") {
                        oss << "  \xE2\x9C\x93 Right:\n"
                            << "    <<javascript -> JSON\n"
                            << "    const data = {key: [1, 2, 3]};\n"
                            << "    console.log(JSON.stringify(data));\n"
                            << "    >>\n";
                    } else {
                        oss << "  Print valid JSON as the LAST line of stdout.\n";
                    }
                    oss << "\n  Key rule: -> JSON requires the last stdout line to be valid JSON.\n";
                    gc_suspended_ = false;
                    throw std::runtime_error(oss.str());
                }
            }

            // Governance: Validate polyglot output format if configured
            if (governance_ && governance_->isActive()) {
                if (auto* str_val = std::get_if<std::string>(&value->data)) {
                    std::string output_err = governance_->checkPolyglotOutput(*str_val);
                    if (!output_err.empty()) {
                        gc_suspended_ = false;
                        throw std::runtime_error(output_err);
                    }
                }

                // BUG-P: Audit logging for parallel polyglot execution
                std::vector<std::string> bound_vars(block.read_vars.begin(), block.read_vars.end());
                governance_->logPolyglotExecution(lang_str, bound_vars, 0,
                    current_file_, block.node ? block.node->getLocation().line : 0);
            }

            // Store result value
            if (!block.assigned_var.empty()) {
                current_env_->define(block.assigned_var, value);
            }
        } else {
            // Handle error - check for undefined variable errors
            std::string error_msg = result.error_message;
            bool is_undefined_var = false;
            std::string var_name;

            // Python: "NameError: name 'x' is not defined"
            if (error_msg.find("NameError") != std::string::npos &&
                error_msg.find("not defined") != std::string::npos) {
                is_undefined_var = true;
                // Try to extract variable name between quotes
                size_t quote1 = error_msg.find('\'');
                if (quote1 != std::string::npos) {
                    size_t quote2 = error_msg.find('\'', quote1 + 1);
                    if (quote2 != std::string::npos) {
                        var_name = error_msg.substr(quote1 + 1, quote2 - quote1 - 1);
                    }
                }
            }

            // JavaScript: "ReferenceError: x is not defined"
            if (!is_undefined_var && error_msg.find("ReferenceError") != std::string::npos &&
                error_msg.find("is not defined") != std::string::npos) {
                is_undefined_var = true;
                // Extract variable name before "is not defined"
                size_t pos = error_msg.find("is not defined");
                if (pos != std::string::npos) {
                    // Look backwards for the variable name
                    std::string prefix = error_msg.substr(0, pos);
                    size_t last_space = prefix.find_last_of(" :");
                    if (last_space != std::string::npos) {
                        var_name = prefix.substr(last_space + 1);
                        // Trim whitespace
                        var_name.erase(0, var_name.find_first_not_of(" \t"));
                        var_name.erase(var_name.find_last_not_of(" \t") + 1);
                    }
                }
            }

            // Python fallback: "name 'x' is not defined" (without NameError: prefix)
            if (!is_undefined_var && error_msg.find("name '") != std::string::npos &&
                error_msg.find("' is not defined") != std::string::npos) {
                is_undefined_var = true;
                size_t q1 = error_msg.find("name '") + 6;
                size_t q2 = error_msg.find("'", q1);
                if (q2 != std::string::npos) var_name = error_msg.substr(q1, q2 - q1);
            }

            // Go: "undefined: x"
            if (!is_undefined_var && error_msg.find("undefined: ") != std::string::npos) {
                is_undefined_var = true;
                size_t pos = error_msg.find("undefined: ");
                var_name = error_msg.substr(pos + 11);
                size_t end = var_name.find_first_of(" \t\n\r");
                if (end != std::string::npos) var_name = var_name.substr(0, end);
            }

            // Ruby: "undefined local variable or method 'x'"
            if (!is_undefined_var && error_msg.find("undefined local variable") != std::string::npos) {
                is_undefined_var = true;
                size_t q1 = error_msg.find("'");
                if (q1 != std::string::npos) {
                    size_t q2 = error_msg.find("'", q1 + 1);
                    if (q2 != std::string::npos) var_name = error_msg.substr(q1 + 1, q2 - q1 - 1);
                }
            }

            // Nim: "undeclared identifier: 'x'"
            if (!is_undefined_var && error_msg.find("undeclared identifier") != std::string::npos) {
                is_undefined_var = true;
                size_t q1 = error_msg.find("'");
                if (q1 != std::string::npos) {
                    size_t q2 = error_msg.find("'", q1 + 1);
                    if (q2 != std::string::npos) var_name = error_msg.substr(q1 + 1, q2 - q1 - 1);
                }
            }

            if (is_undefined_var) {
                std::ostringstream oss;
                oss << "Parallel polyglot execution failed in block " << j << ": " << error_msg << "\n\n";
                oss << "  Help: Did you forget to bind a NAAb variable?\n";
                oss << "  Inline polyglot code requires explicit variable binding syntax.\n\n";

                if (!var_name.empty()) {
                    oss << "  ✗ Wrong - variable not bound:\n";
                    oss << "    let result = <<" << lang_str << "\n";
                    oss << "    " << var_name << " * 2\n";
                    oss << "    >>\n\n";
                    oss << "  ✓ Right - explicit variable binding:\n";
                    oss << "    let result = <<" << lang_str << "[" << var_name << "]\n";
                    oss << "    " << var_name << " * 2\n";
                    oss << "    >>\n\n";
                } else {
                    oss << "  Syntax: <<language[var1, var2, ...]\n";
                    oss << "    your code here\n";
                    oss << "  >>\n\n";
                }

                oss << "  Example with multiple variables:\n";
                oss << "    let a = 10\n";
                oss << "    let b = 20\n";
                oss << "    let sum = <<" << lang_str << "[a, b]\n";
                oss << "    a + b\n";
                oss << "    >>\n";

                gc_suspended_ = false;
                throw std::runtime_error(oss.str());
            }

            // Phase 12: Translate temp file paths to NAAb source locations
            if (block.node) {
                runtime::SourceMapper source_mapper(current_file_,
                    block.node->getLocation().line, block.node->getLocation().column);
                std::string translated = source_mapper.translateError(error_msg);
                if (!translated.empty() && translated != error_msg) {
                    error_msg = translated + "\n  Original error: " + error_msg;
                }
            }

            // Detect common polyglot errors and add helpful context
            std::ostringstream oss;
            oss << "Parallel polyglot execution failed in block " << j << ": " << error_msg << "\n";

            if (error_msg.find("IndentationError") != std::string::npos ||
                error_msg.find("unexpected indent") != std::string::npos) {
                oss << "\n  Help: Python indentation error in polyglot block.\n"
                    << "  All lines should use consistent indentation (spaces, not tabs).\n"
                    << "  NAAb strips common leading whitespace, but mixed indentation breaks Python.\n";
            }
            else if (lang_str == "python" && error_msg.find("SyntaxError") != std::string::npos) {
                oss << "\n  Help: Python syntax error. Check colons, brackets, and Python 3 syntax.\n"
                    << "  The last expression in the block is the return value.\n";
            }
            else if (error_msg.find("ModuleNotFoundError") != std::string::npos ||
                     error_msg.find("ImportError") != std::string::npos) {
                oss << "\n  Help: Missing Python module. Install with: pip install <module>\n";
            }
            else if (error_msg.find("compilation failed") != std::string::npos) {
                oss << "\n  Help: " << lang_str << " compilation failed. Check syntax and compiler installation.\n";
            }
            // JavaScript: 'return' keyword in expression
            else if (lang_str == "javascript" &&
                     (error_msg.find("unexpected token") != std::string::npos ||
                      error_msg.find("SyntaxError") != std::string::npos) &&
                     error_msg.find("return") != std::string::npos) {
                oss << "\n  Help: Don't use 'return' in JavaScript polyglot blocks.\n"
                    << "  The last expression is automatically returned to NAAb.\n\n"
                    << "  ✗ Wrong:  return 42\n"
                    << "  ✓ Right:  42\n";
            }

            gc_suspended_ = false;
            throw std::runtime_error(oss.str());
        }
    }

    gc_suspended_ = false;
}

// Phase 2.2: Serialize a value for injection into target language
std::string Interpreter::serializeValueForLanguage(const std::shared_ptr<Value>& value, const std::string& language) {
    if (!value) {
        return "null";
    }

    // Int
    if (std::holds_alternative<int>(value->data)) {
        return std::to_string(std::get<int>(value->data));
    }

    // Float — use %.15g to match Value::toString() (not std::to_string which gives 6 decimals)
    if (std::holds_alternative<double>(value->data)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", std::get<double>(value->data));
        return std::string(buf);
    }

    // String
    if (std::holds_alternative<std::string>(value->data)) {
        const auto& str = std::get<std::string>(value->data);

        // FIX 20: Shell — use single-quote wrapping (ZERO metacharacter expansion)
        // Only ' itself needs escaping: ' → '\'' (end quote, escaped literal, restart)
        if (language == "shell" || language == "sh" || language == "bash") {
            std::string escaped;
            for (char c : str) {
                if (c == '\'') {
                    escaped += "'\\''";
                } else {
                    escaped += c;
                }
            }
            return "'" + escaped + "'";
        }

        // Other languages need quoted strings with escaping
        std::string escaped;
        for (char c : str) {
            if (c == '"') escaped += "\\\"";
            else if (c == '\\') escaped += "\\\\";
            else if (c == '\n') escaped += "\\n";
            else if (c == '\r') escaped += "\\r";
            else if (c == '\t') escaped += "\\t";
            else if (c == '\0') escaped += "\\0";
            else escaped += c;
        }
        return "\"" + escaped + "\"";
    }

    // Bool — language-specific true/false literals
    if (std::holds_alternative<bool>(value->data)) {
        bool b = std::get<bool>(value->data);
        if (language == "python") return b ? "True" : "False";
        if (language == "shell" || language == "sh" || language == "bash") return b ? "1" : "0";
        return b ? "true" : "false";  // JS, Go, Ruby, Nim, Julia, Rust, C#, PHP, default
    }

    // Null/void — language-specific null literals
    if (std::holds_alternative<std::monostate>(value->data)) {
        if (language == "python") return "None";
        if (language == "go") return "nil";
        if (language == "ruby") return "nil";
        if (language == "nim") return "\"\"";       // Nim's nil is pointer-only; use empty string
        if (language == "julia") return "nothing";
        if (language == "rust") return "\"\"";       // Rust has no generic null; use empty string
        if (language == "shell" || language == "sh" || language == "bash") return "\"\"";
        if (language == "cpp" || language == "c++") return "\"\"";
        if (language == "php") return "null";
        if (language == "csharp" || language == "cs") return "null";
        return "null";  // JS, TS, and default
    }

    // List - language-specific array serialization
    if (std::holds_alternative<std::vector<std::shared_ptr<Value>>>(value->data)) {
        const auto& list = std::get<std::vector<std::shared_ptr<Value>>>(value->data);

        // PHP: array() syntax
        if (language == "php") {
            std::string result = "array(";
            for (size_t i = 0; i < list.size(); i++) {
                if (i > 0) result += ", ";
                result += serializeValueForLanguage(list[i], language);
            }
            result += ")";
            return result;
        }

        // Rust: vec![] macro
        if (language == "rust") {
            std::string result = "vec![";
            for (size_t i = 0; i < list.size(); i++) {
                if (i > 0) result += ", ";
                result += serializeValueForLanguage(list[i], language);
            }
            result += "]";
            return result;
        }

        // Go: []interface{}{}
        if (language == "go") {
            std::string result = "[]interface{}{";
            for (size_t i = 0; i < list.size(); i++) {
                if (i > 0) result += ", ";
                result += serializeValueForLanguage(list[i], language);
            }
            result += "}";
            return result;
        }

        // C#: new List<object>{}
        if (language == "csharp" || language == "cs") {
            std::string result = "new System.Collections.Generic.List<object>{";
            for (size_t i = 0; i < list.size(); i++) {
                if (i > 0) result += ", ";
                result += serializeValueForLanguage(list[i], language);
            }
            result += "}";
            return result;
        }

        // C++: std::vector with initializer list
        if (language == "cpp" || language == "c++") {
            // For C++, we use initializer list but need to figure out element type
            std::string result = "std::vector<std::string>{";
            for (size_t i = 0; i < list.size(); i++) {
                if (i > 0) result += ", ";
                // Serialize all as strings for simplicity
                auto elem_str = serializeValueForLanguage(list[i], language);
                result += elem_str;
            }
            result += "}";
            return result;
        }

        // Nim: @[] sequence literal
        if (language == "nim") {
            std::string result = "@[";
            for (size_t i = 0; i < list.size(); i++) {
                if (i > 0) result += ", ";
                result += serializeValueForLanguage(list[i], language);
            }
            result += "]";
            return result;
        }

        // Default: JSON-like array (Python, JS, TS, Ruby, Shell, Julia, Zig)
        std::string result = "[";
        for (size_t i = 0; i < list.size(); i++) {
            if (i > 0) result += ", ";
            result += serializeValueForLanguage(list[i], language);
        }
        result += "]";
        return result;
    }

    // Dict - language-specific serialization
    if (std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data)) {
        const auto& dict = std::get<std::unordered_map<std::string, std::shared_ptr<Value>>>(value->data);

        // FIX-2: Helper to escape dict keys (prevents broken/injectable code in target languages)
        auto escapeKey = [](const std::string& k) -> std::string {
            std::string escaped;
            for (char c : k) {
                if (c == '"') escaped += "\\\"";
                else if (c == '\\') escaped += "\\\\";
                else if (c == '\n') escaped += "\\n";
                else if (c == '\r') escaped += "\\r";
                else if (c == '\t') escaped += "\\t";
                else escaped += c;
            }
            return escaped;
        };

        // Ruby: use hash rocket syntax {key => value}
        if (language == "ruby") {
            std::string result = "{";
            bool first = true;
            for (const auto& [key, val] : dict) {
                if (!first) result += ", ";
                first = false;
                result += "\"" + escapeKey(key) + "\" => " + serializeValueForLanguage(val, language);
            }
            result += "}";
            return result;
        }

        // PHP: array("key" => "value")
        if (language == "php") {
            std::string result = "array(";
            bool first = true;
            for (const auto& [key, val] : dict) {
                if (!first) result += ", ";
                first = false;
                result += "\"" + escapeKey(key) + "\" => " + serializeValueForLanguage(val, language);
            }
            result += ")";
            return result;
        }

        // Go: map[string]interface{}{}
        if (language == "go") {
            std::string result = "map[string]interface{}{";
            bool first = true;
            for (const auto& [key, val] : dict) {
                if (!first) result += ", ";
                first = false;
                result += "\"" + escapeKey(key) + "\": " + serializeValueForLanguage(val, language);
            }
            result += "}";
            return result;
        }

        // Rust: HashMap (use context file for complex, but inline for simple)
        if (language == "rust") {
            // Generate a block expression that creates a HashMap
            std::string result = "{ let mut __m = std::collections::HashMap::new(); ";
            for (const auto& [key, val] : dict) {
                result += "__m.insert(\"" + escapeKey(key) + "\".to_string(), " + serializeValueForLanguage(val, language) + "); ";
            }
            result += "__m }";
            return result;
        }

        // C#: Dictionary
        if (language == "csharp" || language == "cs") {
            std::string result = "new System.Collections.Generic.Dictionary<string, object>{";
            bool first = true;
            for (const auto& [key, val] : dict) {
                if (!first) result += ", ";
                first = false;
                result += "{\"" + escapeKey(key) + "\", " + serializeValueForLanguage(val, language) + "}";
            }
            result += "}";
            return result;
        }

        // C++: std::map
        if (language == "cpp" || language == "c++") {
            std::string result = "std::map<std::string, std::string>{";
            bool first = true;
            for (const auto& [key, val] : dict) {
                if (!first) result += ", ";
                first = false;
                result += "{\"" + escapeKey(key) + "\", " + serializeValueForLanguage(val, language) + "}";
            }
            result += "}";
            return result;
        }

        // Default: JSON-like object (Python, JS, TS)
        std::string result = "{";
        bool first = true;
        for (const auto& [key, val] : dict) {
            if (!first) result += ", ";
            first = false;
            result += "\"" + escapeKey(key) + "\": " + serializeValueForLanguage(val, language);
        }
        result += "}";
        return result;
    }

    // Struct - serialize as JSON object with field names
    if (std::holds_alternative<std::shared_ptr<StructValue>>(value->data)) {
        const auto& struct_val = std::get<std::shared_ptr<StructValue>>(value->data);
        std::string result = "{";
        bool first = true;
        for (size_t i = 0; i < struct_val->definition->fields.size(); i++) {
            if (!first) result += ", ";
            first = false;
            const auto& field = struct_val->definition->fields[i];
            result += "\"" + field.name + "\": " + serializeValueForLanguage(struct_val->field_values[i], language);
        }
        result += "}";
        return result;
    }

    // Unsupported types
    return "null";
}


} // namespace interpreter
} // namespace naab
