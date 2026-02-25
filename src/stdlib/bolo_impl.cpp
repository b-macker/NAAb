// NAAb BOLO Stdlib Module
// Provides bolo.scan(), bolo.load_profile(), etc. for .naab scripts
// Wraps the C++ GovernanceEngine for use from NAAb code

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/governance.h"
#include <memory>
#include <unordered_map>

namespace naab {
namespace stdlib {

using namespace governance;

using DictType = std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>;
using ArrayType = std::vector<std::shared_ptr<interpreter::Value>>;

// Helper functions (same pattern as env_impl.cpp)
static std::string getString(const std::shared_ptr<interpreter::Value>& val) {
    return std::get<std::string>(val->data);
}

static std::shared_ptr<interpreter::Value> makeString(const std::string& s) {
    return std::make_shared<interpreter::Value>(s);
}

static std::shared_ptr<interpreter::Value> makeDouble(double d) {
    return std::make_shared<interpreter::Value>(d);
}

static std::shared_ptr<interpreter::Value> makeBool(bool b) {
    return std::make_shared<interpreter::Value>(b);
}

static std::shared_ptr<interpreter::Value> makeNull() {
    return std::make_shared<interpreter::Value>();
}

static std::shared_ptr<interpreter::Value> makeArray(ArrayType arr) {
    return std::make_shared<interpreter::Value>(std::move(arr));
}

static std::shared_ptr<interpreter::Value> makeDict(DictType d) {
    return std::make_shared<interpreter::Value>(std::move(d));
}

// Singleton governance engine
static std::unique_ptr<GovernanceEngine> g_engine;
static std::string g_current_profile = "enterprise";

// Built-in profile names
static const std::unordered_map<std::string, std::string> BUILT_IN_PROFILES = {
    {"enterprise", "all"},
    {"llm", "llm"},
    {"security", "security"},
    {"ai-governance", "ai-governance"},
    {"standard", "standard"},
};

static std::string levelStr(EnforcementLevel level) {
    switch (level) {
        case EnforcementLevel::HARD: return "error";
        case EnforcementLevel::SOFT: return "warning";
        case EnforcementLevel::ADVISORY: return "info";
        default: return "unknown";
    }
}

static void ensureEngine() {
    if (!g_engine) {
        g_engine = std::make_unique<GovernanceEngine>();
    }
    auto& rules = g_engine->getMutableRules();
    rules.mode = GovernanceMode::AUDIT;
}

static void applyProfile(const std::string& profile) {
    ensureEngine();
    auto& rules = g_engine->getMutableRules();

    bool enable_llm = (profile == "enterprise" || profile == "llm" ||
                       profile == "ai-governance" || profile == "standard");
    bool enable_security = (profile == "enterprise" || profile == "security" ||
                            profile == "standard");
    bool enable_quality = (profile == "enterprise" || profile == "llm" ||
                           profile == "standard");
    bool enable_all = (profile == "enterprise");

    // Legacy flat fields
    rules.no_secrets = enable_security || enable_all;
    rules.no_secrets_level = EnforcementLevel::HARD;
    rules.no_placeholders = enable_quality || enable_all;
    rules.no_placeholders_level = EnforcementLevel::SOFT;
    rules.no_hardcoded_results = enable_llm || enable_all;
    rules.no_hardcoded_results_level = EnforcementLevel::ADVISORY;
    rules.restrict_dangerous_calls = enable_security || enable_all;
    rules.dangerous_calls_level = EnforcementLevel::HARD;

    // v3.0 code quality
    rules.code_quality.no_secrets.enabled = rules.no_secrets;
    rules.code_quality.no_placeholders.enabled = rules.no_placeholders;
    rules.code_quality.no_hardcoded_results.enabled = rules.no_hardcoded_results;
    rules.code_quality.no_pii.enabled = enable_security || enable_all;
    rules.code_quality.no_temporary_code.enabled = enable_quality || enable_all;
    rules.code_quality.no_simulation_markers.enabled = enable_llm || enable_all;
    rules.code_quality.no_mock_data.enabled = enable_llm || enable_all;
    rules.code_quality.no_apologetic_language.enabled = enable_llm || enable_all;
    rules.code_quality.no_dead_code.enabled = enable_all;
    rules.code_quality.no_debug_artifacts.enabled = enable_security || enable_all;
    rules.code_quality.no_unsafe_deserialization.enabled = enable_security || enable_all;
    rules.code_quality.no_sql_injection.enabled = enable_security || enable_all;
    rules.code_quality.no_path_traversal.enabled = enable_security || enable_all;
    rules.code_quality.no_hardcoded_urls.enabled = enable_all;
    rules.code_quality.no_hardcoded_ips.enabled = enable_all;
    rules.code_quality.encoding.enabled = enable_all;
    rules.code_quality.no_oversimplification.enabled = enable_llm || enable_all;
    rules.code_quality.no_incomplete_logic.enabled = enable_llm || enable_all;
    rules.code_quality.no_hallucinated_apis.enabled = enable_llm || enable_all;

    // v3.0 security restrictions
    rules.restrictions.shell_injection.enabled = enable_security || enable_all;
    rules.restrictions.data_exfiltration.enabled = enable_security || enable_all;
    rules.restrictions.privilege_escalation.enabled = enable_security || enable_all;
    rules.restrictions.information_disclosure.enabled = enable_security || enable_all;
    rules.restrictions.code_injection.enabled = enable_security || enable_all;
    rules.restrictions.crypto.enabled = enable_security || enable_all;

    g_current_profile = profile;
}

static std::shared_ptr<interpreter::Value> resultToDict(const CheckResult& r) {
    DictType d;
    d["rule"] = makeString(r.rule_name);
    d["message"] = makeString(r.message);
    d["passed"] = makeBool(r.passed);
    d["category"] = makeString(r.category);
    d["severity"] = makeString(r.severity);
    d["level"] = makeString(levelStr(r.level));
    d["line"] = makeDouble(static_cast<double>(r.line));
    return makeDict(std::move(d));
}

bool BoloModule::hasFunction(const std::string& name) const {
    return name == "scan" || name == "load_profile" || name == "load_config" ||
           name == "check_count" || name == "profiles" || name == "reset" ||
           name == "violations" || name == "summary";
}

std::shared_ptr<interpreter::Value> BoloModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // bolo.scan(language, code) -> array of violation dicts
    if (function_name == "scan") {
        if (args.size() < 2) {
            throw std::runtime_error(
                "bolo.scan() error: Expected 2 arguments (language, code)\n\n"
                "  Got: " + std::to_string(args.size()) + " argument(s)\n\n"
                "  Example:\n"
                "    let violations = bolo.scan(\"python\", code)\n");
        }
        std::string lang = getString(args[0]);
        std::string code = getString(args[1]);

        ensureEngine();
        g_engine->resetCheckResults();
        g_engine->checkPolyglotBlock(lang, code, "<bolo-scan>", 1);

        ArrayType results;
        for (const auto& r : g_engine->getCheckResults()) {
            if (!r.passed) {
                results.push_back(resultToDict(r));
            }
        }
        return makeArray(std::move(results));
    }

    // bolo.load_profile(name) -> null
    if (function_name == "load_profile") {
        if (args.empty()) {
            throw std::runtime_error(
                "bolo.load_profile() error: Expected 1 argument (profile name)\n\n"
                "  Available profiles: enterprise, llm, security, ai-governance, standard\n");
        }
        applyProfile(getString(args[0]));
        return makeNull();
    }

    // bolo.load_config(path) -> null
    if (function_name == "load_config") {
        if (args.empty()) {
            throw std::runtime_error(
                "bolo.load_config() error: Expected 1 argument (config file path)\n");
        }
        ensureEngine();
        std::string path = getString(args[0]);
        if (!g_engine->loadFromFile(path)) {
            throw std::runtime_error(
                "bolo.load_config() error: Failed to load config from: " + path + "\n");
        }
        return makeNull();
    }

    // bolo.check_count() -> number
    if (function_name == "check_count") {
        ensureEngine();
        int count = 0;
        const auto& rules = g_engine->getRules();

        if (rules.no_secrets) count++;
        if (rules.no_placeholders) count++;
        if (rules.no_hardcoded_results) count++;
        if (rules.restrict_dangerous_calls) count++;
        if (rules.code_quality.no_pii.enabled) count++;
        if (rules.code_quality.no_temporary_code.enabled) count++;
        if (rules.code_quality.no_simulation_markers.enabled) count++;
        if (rules.code_quality.no_mock_data.enabled) count++;
        if (rules.code_quality.no_apologetic_language.enabled) count++;
        if (rules.code_quality.no_dead_code.enabled) count++;
        if (rules.code_quality.no_debug_artifacts.enabled) count++;
        if (rules.code_quality.no_unsafe_deserialization.enabled) count++;
        if (rules.code_quality.no_sql_injection.enabled) count++;
        if (rules.code_quality.no_path_traversal.enabled) count++;
        if (rules.code_quality.no_hardcoded_urls.enabled) count++;
        if (rules.code_quality.no_hardcoded_ips.enabled) count++;
        if (rules.code_quality.encoding.enabled) count++;
        if (rules.code_quality.no_oversimplification.enabled) count++;
        if (rules.code_quality.no_incomplete_logic.enabled) count++;
        if (rules.code_quality.no_hallucinated_apis.enabled) count++;
        if (rules.restrictions.shell_injection.enabled) count++;
        if (rules.restrictions.data_exfiltration.enabled) count++;
        if (rules.restrictions.privilege_escalation.enabled) count++;
        if (rules.restrictions.information_disclosure.enabled) count++;
        if (rules.restrictions.code_injection.enabled) count++;
        if (rules.restrictions.crypto.enabled) count++;

        return makeDouble(static_cast<double>(count));
    }

    // bolo.profiles() -> array of strings
    if (function_name == "profiles") {
        ArrayType result;
        for (const auto& [name, _] : BUILT_IN_PROFILES) {
            result.push_back(makeString(name));
        }
        return makeArray(std::move(result));
    }

    // bolo.reset() -> null
    if (function_name == "reset") {
        if (g_engine) {
            g_engine->resetCheckResults();
        }
        return makeNull();
    }

    // bolo.violations() -> array of all check results
    if (function_name == "violations") {
        ensureEngine();
        ArrayType results;
        for (const auto& r : g_engine->getCheckResults()) {
            results.push_back(resultToDict(r));
        }
        return makeArray(std::move(results));
    }

    // bolo.summary() -> string
    if (function_name == "summary") {
        ensureEngine();
        return makeString(g_engine->formatSummary());
    }

    throw std::runtime_error("bolo module: Unknown function '" + function_name + "'");
}

} // namespace stdlib
} // namespace naab
