// codegen_impl.cpp — Governed dynamic code execution via polyglot executors
//
// Routes dynamic code strings through the same 39+ governance checks as
// static <<language ... >> blocks, with additional defenses for runtime-
// generated code (taint, size limits, nesting, action matrix).

#include "naab/stdlib_new_modules.h"
#include "naab/language_registry.h"
#include "naab/governance.h"
#include "naab/sandbox.h"
#include "naab/resource_limits.h"
#include "naab/naab_val.h"

#include <algorithm>
#include <chrono>
#include <regex>
#include <unordered_map>

namespace naab {
namespace stdlib {

// --- Thread-local taint flag (set by VM/interpreter before module dispatch) ---
static thread_local bool t_codegen_arg_tainted = false;

// --- Cumulative counters ---
static thread_local int t_codegen_call_count = 0;
static thread_local int t_codegen_blocked_count = 0;
static thread_local int64_t t_codegen_total_duration_ms = 0;
static thread_local size_t t_codegen_cumulative_bytes = 0;
static thread_local int t_codegen_nesting_depth = 0;
static thread_local std::unordered_map<int, int> t_codegen_per_agent_calls;

// --- Nesting guard ---
struct ScopedCodegenDepth {
    ScopedCodegenDepth() { ++t_codegen_nesting_depth; }
    ~ScopedCodegenDepth() { --t_codegen_nesting_depth; }
};

// External: agent tool context from agent_impl.cpp
extern const governance::AgentConfig* getToolAgentContext();

// Public setter for taint plumbing from VM/interpreter
void setCodegenArgTainted(bool v) { t_codegen_arg_tainted = v; }

// Dashboard stats accessor
CodegenStats getCodegenStats() {
    return {t_codegen_call_count, t_codegen_blocked_count, t_codegen_total_duration_ms};
}

// --- Normalize language name ---
static std::string normalizeLanguage(const std::string& lang) {
    std::string normalized = lang;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    if (normalized == "js") return "javascript";
    if (normalized == "bash" || normalized == "sh") return "shell";
    if (normalized == "c++" || normalized == "cxx") return "cpp";
    if (normalized == "c#" || normalized == "csharp") return "cs";
    if (normalized == "py") return "python";
    if (normalized == "rb") return "ruby";
    return normalized;
}

// --- Count lines in code string ---
static int countLines(const std::string& code) {
    if (code.empty()) return 0;
    int count = 1;
    for (char c : code) {
        if (c == '\n') count++;
    }
    return count;
}

// --- Sanitize stderr output (Gap 9) ---
static std::string sanitizeStderr(const std::string& stderr_str, int max_chars) {
    std::string result = stderr_str;

    // Strip absolute paths (Unix)
    result = std::regex_replace(result, std::regex("/[a-zA-Z0-9_./-]{5,}/"), "[path]/");
    // Strip Python traceback file paths
    result = std::regex_replace(result, std::regex("File \"[^\"]+\""), "File \"[internal]\"");
    // Strip Go paths
    result = std::regex_replace(result, std::regex("GOPATH=[^ \\n]+"), "GOPATH=[internal]");
    result = std::regex_replace(result, std::regex("GOROOT=[^ \\n]+"), "GOROOT=[internal]");

    // Truncate
    if (max_chars > 0 && static_cast<int>(result.size()) > max_chars) {
        result = result.substr(0, static_cast<size_t>(max_chars)) + "\n... (truncated)";
    }

    return result;
}

bool CodegenModule::hasFunction(const std::string& name) const {
    return name == "run" || name == "run_with_args" ||
           name == "supported_languages" || name == "is_enabled";
}

interpreter::NaabVal CodegenModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    auto* gov_engine = governance::GovernanceEngine::getCurrent();

    // ── codegen.is_enabled() ──
    if (function_name == "is_enabled") {
        if (!gov_engine || !gov_engine->isActive())
            return interpreter::NaabVal::makeBool(false);
        return interpreter::NaabVal::makeBool(gov_engine->getCodegenEnabled());
    }

    // ── codegen.supported_languages() ──
    if (function_name == "supported_languages") {
        auto& registry = runtime::LanguageRegistry::instance();
        auto languages = registry.supportedLanguages();

        // Filter by governance if active
        if (gov_engine && gov_engine->isActive()) {
            const auto& codegen_cfg = gov_engine->getCodegenConfig();
            if (!codegen_cfg.allowed_languages.empty()) {
                std::vector<std::string> filtered;
                for (const auto& lang : languages) {
                    if (std::find(codegen_cfg.allowed_languages.begin(),
                                  codegen_cfg.allowed_languages.end(), lang)
                        != codegen_cfg.allowed_languages.end()) {
                        filtered.push_back(lang);
                    }
                }
                languages = filtered;
            }
            // Remove blocked languages
            for (const auto& blocked : codegen_cfg.blocked_languages) {
                languages.erase(std::remove(languages.begin(), languages.end(), blocked),
                               languages.end());
            }
        }

        std::vector<interpreter::NaabVal> result;
        for (const auto& lang : languages) {
            result.push_back(interpreter::NaabVal::makeString(lang));
        }
        return interpreter::NaabVal::makeList(std::move(result));
    }

    // ── codegen.run(language, code) / codegen.run_with_args(language, code, bindings) ──
    if (function_name == "run" || function_name == "run_with_args") {

        // Step 0: Argument validation
        if (args.size() < 2) {
            throw std::runtime_error(
                "Codegen error: codegen." + function_name + "() requires at least 2 arguments\n\n"
                "  Got: " + std::to_string(args.size()) + " argument(s)\n"
                "  Expected: codegen." + function_name + "(language, code)\n\n"
                "  Example:\n"
                "    codegen.run(\"python\", \"print(42)\")\n");
        }
        if (!args[0].isString()) {
            throw std::runtime_error(
                "Codegen error: first argument must be a language name string\n\n"
                "  Got: " + args[0].getTypeName() + "\n"
                "  Expected: string (e.g., \"python\", \"javascript\", \"go\")\n");
        }
        if (!args[1].isString()) {
            throw std::runtime_error(
                "Codegen error: second argument must be a code string\n\n"
                "  Got: " + args[1].getTypeName() + "\n"
                "  Expected: string containing source code\n");
        }

        std::string language = normalizeLanguage(args[0].asString());
        std::string code = args[1].asString();

        // Variable bindings for run_with_args
        std::unordered_map<std::string, interpreter::NaabVal> bindings;
        if (function_name == "run_with_args") {
            if (args.size() < 3 || !args[2].isDict()) {
                throw std::runtime_error(
                    "Codegen error: codegen.run_with_args() requires a dict as third argument\n\n"
                    "  Expected: codegen.run_with_args(language, code, {\"var\": value})\n");
            }
            for (const auto& [k, v] : args[2].asDictConst()) {
                bindings[k] = v;
            }
        }

        // Gap 10: Reject null bytes in code string
        if (code.find('\0') != std::string::npos) {
            throw std::runtime_error(
                "Codegen error: code string contains null bytes\n\n"
                "  Null bytes are not permitted in dynamic code strings.\n");
        }

        // Get codegen config
        governance::CodegenConfig codegen_cfg;
        if (gov_engine && gov_engine->isActive()) {
            codegen_cfg = gov_engine->getCodegenConfig();
        }

        // Step 1: Master switch check
        if (gov_engine && gov_engine->isActive() && !codegen_cfg.enabled) {
            // Emit CODEGEN_BLOCKED event
            t_codegen_blocked_count++;
            gov_engine->emitEvent(governance::RuntimeEventType::CODEGEN_BLOCKED,
                "codegen.run('" + language + "') blocked: not enabled", "", 0);
            throw std::runtime_error(
                "Codegen error: dynamic code execution is not enabled\n\n"
                "  codegen.run() requires explicit enablement in governance configuration.\n"
                "  Use static polyglot blocks instead: <<" + language + " ... >>\n");
        }

        // Step 2: CODEGEN_EXEC action matrix check (Gap 7)
        const auto* agent_ctx = getToolAgentContext();
        if (agent_ctx && !agent_ctx->allowed_actions.empty()) {
            bool codegen_allowed = false;
            for (const auto& action : agent_ctx->allowed_actions) {
                if (action == "CODEGEN_EXEC") { codegen_allowed = true; break; }
            }
            if (!codegen_allowed) {
                t_codegen_blocked_count++;
                if (gov_engine) {
                    gov_engine->emitEvent(governance::RuntimeEventType::CODEGEN_BLOCKED,
                        "codegen.run('" + language + "') blocked: CODEGEN_EXEC not in allowed_actions",
                        "", 0);
                }
                throw std::runtime_error(
                    "Codegen error: dynamic code execution is not permitted for this agent\n\n"
                    "  The calling agent's action restrictions do not include dynamic code execution.\n");
            }
        }

        // Step 3: Taint check on code_string (Gap 1)
        if (gov_engine && gov_engine->isActive()) {
            if (t_codegen_arg_tainted && !codegen_cfg.allow_tainted_code) {
                t_codegen_blocked_count++;
                gov_engine->emitEvent(governance::RuntimeEventType::CODEGEN_BLOCKED,
                    "codegen.run('" + language + "') blocked: tainted code string",
                    "", 0);
                throw std::runtime_error(
                    "Codegen error: cannot execute tainted code string\n\n"
                    "  The code string originates from an untrusted source (LLM output,\n"
                    "  file read, or other taint source). Executing tainted code is blocked\n"
                    "  by default for safety.\n");
            }
        }
        // Reset taint flag after checking
        t_codegen_arg_tainted = false;

        // Step 4: Code size + cumulative limits (Gap 3, Gap 5)
        if (gov_engine && gov_engine->isActive()) {
            int code_size = static_cast<int>(code.size());
            int code_lines = countLines(code);

            if (codegen_cfg.max_code_size_bytes > 0 && code_size > codegen_cfg.max_code_size_bytes) {
                throw std::runtime_error(
                    "Codegen error: code exceeds maximum size\n\n"
                    "  Got: " + std::to_string(code_size) + " bytes\n"
                    "  Limit: " + std::to_string(codegen_cfg.max_code_size_bytes) + " bytes\n");
            }
            if (codegen_cfg.max_code_lines > 0 && code_lines > codegen_cfg.max_code_lines) {
                throw std::runtime_error(
                    "Codegen error: code exceeds maximum line count\n\n"
                    "  Got: " + std::to_string(code_lines) + " lines\n"
                    "  Limit: " + std::to_string(codegen_cfg.max_code_lines) + " lines\n");
            }

            // Cumulative checks
            if (codegen_cfg.max_cumulative_calls > 0 &&
                t_codegen_call_count >= codegen_cfg.max_cumulative_calls) {
                throw std::runtime_error(
                    "Codegen error: cumulative call limit reached\n\n"
                    "  Calls made: " + std::to_string(t_codegen_call_count) + "\n"
                    "  Limit: " + std::to_string(codegen_cfg.max_cumulative_calls) + "\n");
            }
            if (codegen_cfg.max_cumulative_code_bytes > 0 &&
                static_cast<int>(t_codegen_cumulative_bytes) + code_size > codegen_cfg.max_cumulative_code_bytes) {
                throw std::runtime_error(
                    "Codegen error: cumulative code size limit reached\n\n"
                    "  Total bytes: " + std::to_string(t_codegen_cumulative_bytes + code.size()) + "\n"
                    "  Limit: " + std::to_string(codegen_cfg.max_cumulative_code_bytes) + "\n");
            }

            // Per-agent call limit
            if (agent_ctx && codegen_cfg.max_cumulative_calls_per_agent > 0) {
                // Use a simple hash of agent context pointer as handle proxy
                int agent_key = static_cast<int>(reinterpret_cast<uintptr_t>(agent_ctx) & 0xFFFFFF);
                if (t_codegen_per_agent_calls[agent_key] >= codegen_cfg.max_cumulative_calls_per_agent) {
                    throw std::runtime_error(
                        "Codegen error: per-agent cumulative call limit reached\n\n"
                        "  Calls by this agent: " + std::to_string(t_codegen_per_agent_calls[agent_key]) + "\n"
                        "  Limit: " + std::to_string(codegen_cfg.max_cumulative_calls_per_agent) + "\n");
                }
            }
        }

        // Step 5: Nesting depth check (Gap 6)
        if (gov_engine && gov_engine->isActive()) {
            int cur_depth = t_codegen_nesting_depth;
            if (cur_depth > codegen_cfg.max_nesting_depth) {
                throw std::runtime_error(
                    "Codegen error: nesting depth exceeded\n\n"
                    "  Current depth: " + std::to_string(cur_depth) + "\n"
                    "  Maximum allowed: " + std::to_string(codegen_cfg.max_nesting_depth) + "\n");
            }
        }

        // Step 6: Language validation (Gap 4)
        if (gov_engine && gov_engine->isActive()) {
            // Check codegen-specific language restrictions
            if (!codegen_cfg.allowed_languages.empty()) {
                if (std::find(codegen_cfg.allowed_languages.begin(),
                              codegen_cfg.allowed_languages.end(), language)
                    == codegen_cfg.allowed_languages.end()) {
                    throw std::runtime_error(
                        "Codegen error: language '" + language + "' is not allowed for dynamic code\n\n"
                        "  Allowed languages for codegen: " +
                        [&]() {
                            std::string s;
                            for (const auto& l : codegen_cfg.allowed_languages) {
                                if (!s.empty()) s += ", ";
                                s += l;
                            }
                            return s;
                        }() + "\n");
                }
            }
            if (!codegen_cfg.blocked_languages.empty()) {
                if (std::find(codegen_cfg.blocked_languages.begin(),
                              codegen_cfg.blocked_languages.end(), language)
                    != codegen_cfg.blocked_languages.end()) {
                    throw std::runtime_error(
                        "Codegen error: language '" + language + "' is blocked for dynamic code\n");
                }
            }
        }

        // Step 7: Sandbox capability check
        auto* sandbox = security::ScopedSandbox::getCurrent();
        if (sandbox && !sandbox->canExecuteCommand("codegen:" + language)) {
            throw std::runtime_error(
                "Codegen error: execution denied by sandbox\n\n"
                "  The current sandbox does not permit code execution.\n");
        }

        // Step 8: Full polyglot governance checks (39+ checks)
        std::string synthetic_source = "<codegen:" + language + ">";
        if (gov_engine && gov_engine->isActive()) {
            std::string block_err = gov_engine->checkPolyglotBlock(
                language, code, synthetic_source, 0, bindings.size());
            if (!block_err.empty()) {
                t_codegen_blocked_count++;
                gov_engine->emitEvent(governance::RuntimeEventType::CODEGEN_BLOCKED,
                    "codegen.run('" + language + "') blocked by governance", "", 0);
                throw std::runtime_error(block_err);
            }
        }

        // Step 9: Shared rate limiter (Gap 8)
        if (gov_engine && gov_engine->isActive()) {
            std::string rate_err = gov_engine->incrementAndCheckPolyglotBlockCount();
            if (!rate_err.empty()) {
                throw std::runtime_error(rate_err);
            }
            bool rate_exceeded = gov_engine->checkPolyglotRate();
            if (rate_exceeded) {
                throw std::runtime_error(
                    "Codegen error: polyglot rate limit exceeded\n\n"
                    "  Dynamic code execution shares the polyglot rate limiter.\n"
                    "  Try again after the rate window resets.\n");
            }
        }

        // Step 10: BSD event emission (pre-execution)
        if (gov_engine && gov_engine->isActive()) {
            gov_engine->emitEvent(governance::RuntimeEventType::CODEGEN_EXEC,
                "codegen.run('" + language + "', <" + std::to_string(code.size()) + " bytes>)",
                synthetic_source, 0);
        }

        // Step 11: Execute via LanguageRegistry
        ScopedCodegenDepth depth_guard;

        auto& registry = runtime::LanguageRegistry::instance();
        auto* executor = registry.getExecutor(language);
        if (!executor) {
            auto available = registry.supportedLanguages();
            std::string avail_str;
            for (const auto& l : available) {
                if (!avail_str.empty()) avail_str += ", ";
                avail_str += l;
            }
            throw std::runtime_error(
                "Codegen error: no executor available for language '" + language + "'\n\n"
                "  Available languages: " + avail_str + "\n");
        }

        // Build variable preamble for run_with_args
        std::string final_code = code;
        if (!bindings.empty()) {
            std::string preamble;
            for (const auto& [name, val] : bindings) {
                std::string serialized = val.toString();
                // Quote strings for injection into code
                if (val.isString()) {
                    // Escape for target language
                    std::string escaped = val.asString();
                    // Basic escaping: replace backslash, quote, newline
                    std::string safe;
                    for (char c : escaped) {
                        if (c == '\\') safe += "\\\\";
                        else if (c == '"') safe += "\\\"";
                        else if (c == '\n') safe += "\\n";
                        else if (c == '\r') safe += "\\r";
                        else if (c == '\t') safe += "\\t";
                        else safe += c;
                    }
                    serialized = "\"" + safe + "\"";
                } else if (val.isBool()) {
                    if (language == "python") {
                        serialized = val.asBool() ? "True" : "False";
                    } else {
                        serialized = val.asBool() ? "true" : "false";
                    }
                } else if (val.isNull()) {
                    if (language == "python") serialized = "None";
                    else if (language == "go") serialized = "nil";
                    else if (language == "ruby") serialized = "nil";
                    else serialized = "null";
                }

                if (language == "python") {
                    preamble += name + " = " + serialized + "\n";
                } else if (language == "javascript") {
                    preamble += "const " + name + " = " + serialized + ";\n";
                } else if (language == "go") {
                    preamble += "var " + name + " = " + serialized + "\n";
                } else if (language == "rust") {
                    preamble += "let " + name + " = " + serialized + ";\n";
                } else if (language == "shell") {
                    preamble += "export " + name + "=" + serialized + "\n";
                } else if (language == "ruby") {
                    preamble += name + " = " + serialized + "\n";
                } else {
                    preamble += "let " + name + " = " + serialized + ";\n";
                }
            }
            final_code = preamble + final_code;
        }

        // Apply timeout
        int timeout = codegen_cfg.timeout_seconds > 0 ? codegen_cfg.timeout_seconds : 15;

        std::string output;
        std::string stderr_output;
        int exit_code = 0;

        auto exec_start = std::chrono::steady_clock::now();
        try {
            security::ResourceLimiter::setExecutionTimeout(static_cast<unsigned int>(timeout));
            interpreter::NaabVal result = executor->executeWithReturn(final_code);
            security::ResourceLimiter::clearTimeout();

            output = executor->getCapturedOutput();
            exit_code = executor->getLastExitCode();

            // If no captured output but result is a string, use that
            if (output.empty() && result.isString()) {
                output = result.asString();
            }
        } catch (const std::exception& e) {
            security::ResourceLimiter::clearTimeout();
            exit_code = 1;
            stderr_output = e.what();
        }

        auto exec_end = std::chrono::steady_clock::now();
        int duration_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(exec_end - exec_start).count());
        if (duration_ms == 0) duration_ms = 1;
        t_codegen_total_duration_ms += duration_ms;

        // Step 12: Stderr sanitization (Gap 9)
        if (codegen_cfg.sanitize_stderr && !stderr_output.empty()) {
            stderr_output = sanitizeStderr(stderr_output, codegen_cfg.max_stderr_chars);
        }

        // Step 13: Result scanning (secrets, PII)
        if (gov_engine && gov_engine->isActive() && !output.empty()) {
            std::string secret_err = gov_engine->checkSecrets(output, 0);
            if (!secret_err.empty()) {
                output = "[REDACTED: codegen output contained potential secrets]";
            }
            std::string pii_err = gov_engine->checkPii(output, 0);
            if (!pii_err.empty()) {
                output = "[REDACTED: codegen output contained potential PII]";
            }
        }

        // Step 14: Taint the result (unconditionally)
        if (gov_engine && gov_engine->isActive()) {
            gov_engine->setLastReturnTainted(true, "codegen.run");
        }

        // Update cumulative counters
        t_codegen_call_count++;
        t_codegen_cumulative_bytes += code.size();
        if (agent_ctx) {
            int agent_key = static_cast<int>(reinterpret_cast<uintptr_t>(agent_ctx) & 0xFFFFFF);
            t_codegen_per_agent_calls[agent_key]++;
        }

        // Step 15: Build result dict
        std::unordered_map<std::string, interpreter::NaabVal> result_dict;
        result_dict["output"] = interpreter::NaabVal::makeString(output);
        result_dict["exit_code"] = interpreter::NaabVal::makeInt(exit_code);
        result_dict["language"] = interpreter::NaabVal::makeString(language);
        result_dict["duration_ms"] = interpreter::NaabVal::makeInt(duration_ms);
        if (!stderr_output.empty()) {
            result_dict["stderr"] = interpreter::NaabVal::makeString(stderr_output);
        }

        // Step 16: Telemetry
        if (gov_engine && gov_engine->isActive()) {
            gov_engine->writeAgentTelemetry("CODEGEN_EXEC", {
                {"language", language},
                {"code_size", std::to_string(code.size())},
                {"duration_ms", std::to_string(duration_ms)},
                {"exit_code", std::to_string(exit_code)},
                {"cumulative_calls", std::to_string(t_codegen_call_count)}
            });
        }

        return interpreter::NaabVal::makeDict(std::move(result_dict));
    }

    throw std::runtime_error(
        "Codegen error: unknown function '" + function_name + "'\n\n"
        "  Available functions: run, run_with_args, supported_languages, is_enabled\n");
}

} // namespace stdlib
} // namespace naab
