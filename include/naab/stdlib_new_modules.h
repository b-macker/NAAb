#pragma once

// New stdlib modules declarations
// Implementations are in src/stdlib/*_impl.cpp files

#include "naab/stdlib.h"
#include <cstdint>
#include <functional>
#include <fstream>
#include <memory>

namespace naab {
namespace interpreter {
class Interpreter;  // Forward declaration for debug module
}
namespace stdlib {

// String Module
class StringModule : public Module {
public:
    std::string getName() const override { return "string"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;

private:
    // String operations (14 functions)
    static interpreter::NaabVal length(
        const std::vector<interpreter::NaabVal>& args);
    static interpreter::NaabVal upper(
        const std::vector<interpreter::NaabVal>& args);
    static interpreter::NaabVal lower(
        const std::vector<interpreter::NaabVal>& args);
    static interpreter::NaabVal trim(
        const std::vector<interpreter::NaabVal>& args);
    static interpreter::NaabVal split(
        const std::vector<interpreter::NaabVal>& args);
    static interpreter::NaabVal join(
        const std::vector<interpreter::NaabVal>& args);
    static interpreter::NaabVal replace(
        const std::vector<interpreter::NaabVal>& args);
    static interpreter::NaabVal substring(
        const std::vector<interpreter::NaabVal>& args);
    static interpreter::NaabVal startswith(
        const std::vector<interpreter::NaabVal>& args);
    static interpreter::NaabVal endswith(
        const std::vector<interpreter::NaabVal>& args);
    static interpreter::NaabVal contains(
        const std::vector<interpreter::NaabVal>& args);
    static interpreter::NaabVal find(
        const std::vector<interpreter::NaabVal>& args);
    static interpreter::NaabVal repeat(
        const std::vector<interpreter::NaabVal>& args);
    static interpreter::NaabVal reverse(
        const std::vector<interpreter::NaabVal>& args);
};

// Array Module
class ArrayModule : public Module {
public:
    // Type for function evaluator callback
    using FunctionEvaluator = std::function<interpreter::NaabVal(
        interpreter::NaabVal fn,
        const std::vector<interpreter::NaabVal>& args)>;

    ArrayModule() = default;
    explicit ArrayModule(FunctionEvaluator evaluator) : evaluator_(std::move(evaluator)) {}

    std::string getName() const override { return "array"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
    bool isMutatingFunction(const std::string& function_name) const override;

    // Set function evaluator (for higher-order functions like map/filter/reduce)
    void setFunctionEvaluator(FunctionEvaluator evaluator) { evaluator_ = std::move(evaluator); }

private:
    FunctionEvaluator evaluator_;
};

// Math Module
class MathModule : public Module {
public:
    std::string getName() const override { return "math"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// Time Module
class TimeModule : public Module {
public:
    std::string getName() const override { return "time"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// Env Module
class EnvModule : public Module {
public:
    // Type for script arguments provider callback
    using ArgsProvider = std::function<std::vector<std::string>()>;

    EnvModule() = default;
    explicit EnvModule(ArgsProvider provider) : args_provider_(std::move(provider)) {}

    std::string getName() const override { return "env"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;

    // Set script arguments provider (for env.get_args())
    void setArgsProvider(ArgsProvider provider) { args_provider_ = std::move(provider); }

private:
    ArgsProvider args_provider_;
};

// CSV Module
class CsvModule : public Module {
public:
    std::string getName() const override { return "csv"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// Regex Module
class RegexModule : public Module {
public:
    std::string getName() const override { return "regex"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// Crypto Module
class CryptoModule : public Module {
public:
    std::string getName() const override { return "crypto"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// File Module
class FileModule : public Module {
public:
    std::string getName() const override { return "file"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// Debug Module
class DebugModule : public Module {
public:
    std::string getName() const override { return "debug"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;

    // Give debug module access to interpreter for scope inspection
    static void setInterpreter(interpreter::Interpreter* interp);

    // Check if a variable name is tainted by governance taint tracking
    static bool checkTainted(const std::string& var_name);
};

// Governance Scoring Module - Script-accessible scoring pipeline
class GovernanceModule : public Module {
public:
    std::string getName() const override { return "governance"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// BOLO Governance Module
class BoloModule : public Module {
public:
    std::string getName() const override { return "bolo"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// Path Module - Path manipulation utilities
class PathModule : public Module {
public:
    std::string getName() const override { return "path"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// Dict Module - Dictionary utilities (keys, values, has_key)
class DictModule : public Module {
public:
    std::string getName() const override { return "dict"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// Log Module - Structured logging with levels, formats, and output targets
class LogModule : public Module {
public:
    std::string getName() const override { return "log"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
private:
    std::string level_  = "info";   // debug|info|warn|error|none
    std::string format_ = "text";   // text|json
    std::string output_ = "stderr"; // stderr|stdout|<filepath>
    std::shared_ptr<std::ofstream> file_stream_;
    bool shouldLog(const std::string& msg_level) const;
    void writeLog(const std::string& level, const std::string& msg);
    std::ostream& getStream();
};

// UUID Module - Random and deterministic UUID generation
class UuidModule : public Module {
public:
    std::string getName() const override { return "uuid"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// Validate Module - Input validation helpers
class ValidateModule : public Module {
public:
    std::string getName() const override { return "validate"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// Process Module - Subprocess management (run, exit, kill, getpid)
class ProcessModule : public Module {
public:
    std::string getName() const override { return "process"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// Agent Module — LLM conversation management with governance enforcement
class AgentModule : public Module {
public:
    std::string getName() const override { return "agent"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;
};

// Agent dispatch stats — run-level counters for dashboard/telemetry
struct AgentDispatchStats {
    int total_calls = 0;
    int total_retries = 0;
    int total_tokens = 0;
    int64_t total_agent_time_ms = 0;
    int consecutive_failures = 0;
    bool hard_stopped = false;
    std::string stop_reason;
    std::vector<std::string> dead_keys;
    // Tool execution stats (aggregated across all agents)
    int total_tool_calls = 0;
    int total_tool_calls_blocked = 0;
    int64_t total_tool_latency_ms = 0;
};

// Returns a snapshot of current run-level dispatch counters
AgentDispatchStats getAgentDispatchStats();

} // namespace stdlib
} // namespace naab

