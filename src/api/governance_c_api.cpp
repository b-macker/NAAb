/*
 * NAAb Governance — C API Bridge
 * Maps the C API (naab_governance.h) to GovernanceEngine C++ internals.
 *
 * Each naab_gov_engine_s owns a GovernanceEngine instance. All public
 * functions NULL-guard the handle and catch exceptions into last_error.
 */

#include "naab/public/naab_governance.h"
#include "naab/governance.h"

#include <nlohmann/json.hpp>
#include <string>
#include <cstring>
#include <unordered_map>
#include <functional>

using naab::governance::GovernanceEngine;

/* --- Internal handle structure --- */
struct naab_gov_engine_s {
    GovernanceEngine engine;
    std::string last_error;
};

/* --- Version --- */
static const char* const NAAB_GOV_VERSION = "0.10.0";

/* --- RAII guard: set/restore thread-local current engine --- */
struct CurrentEngineGuard {
    GovernanceEngine* prev;
    CurrentEngineGuard(GovernanceEngine* eng)
        : prev(GovernanceEngine::getCurrent())
    {
        GovernanceEngine::setCurrent(eng);
    }
    ~CurrentEngineGuard() {
        GovernanceEngine::setCurrent(prev);
    }
};

/* ========== Lifecycle ========== */

naab_gov_engine_t naab_gov_create(void) {
    try {
        return new naab_gov_engine_s();
    } catch (...) {
        return nullptr;
    }
}

void naab_gov_destroy(naab_gov_engine_t engine) {
    delete engine;  /* NULL-safe per C++ spec */
}

/* ========== Configuration ========== */

naab_gov_error_t naab_gov_load_config(naab_gov_engine_t engine,
                                       const char* path) {
    if (!engine) return NAAB_GOV_ERR_NULL_ARG;
    if (!path)   return NAAB_GOV_ERR_NULL_ARG;
    try {
        if (!engine->engine.loadFromFile(path)) {
            engine->last_error = "Failed to load config: " + engine->engine.getLastError();
            return NAAB_GOV_ERR_CONFIG;
        }
        engine->last_error.clear();
        return NAAB_GOV_OK;
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return NAAB_GOV_ERR_CONFIG;
    }
}

naab_gov_error_t naab_gov_discover_config(naab_gov_engine_t engine,
                                            const char* dir) {
    if (!engine) return NAAB_GOV_ERR_NULL_ARG;
    if (!dir)    return NAAB_GOV_ERR_NULL_ARG;
    try {
        if (!engine->engine.discoverAndLoad(dir)) {
            engine->last_error = "No govern.json found from: " + std::string(dir);
            return NAAB_GOV_ERR_CONFIG;
        }
        engine->last_error.clear();
        return NAAB_GOV_OK;
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return NAAB_GOV_ERR_CONFIG;
    }
}

naab_gov_error_t naab_gov_load_config_string(naab_gov_engine_t engine,
                                               const char* json_config) {
    if (!engine)      return NAAB_GOV_ERR_NULL_ARG;
    if (!json_config) return NAAB_GOV_ERR_NULL_ARG;
    try {
        if (!engine->engine.loadFromString(json_config)) {
            engine->last_error = "Failed to parse config string: " +
                                 engine->engine.getLastError();
            return NAAB_GOV_ERR_CONFIG;
        }
        engine->last_error.clear();
        return NAAB_GOV_OK;
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return NAAB_GOV_ERR_CONFIG;
    }
}

int naab_gov_is_active(naab_gov_engine_t engine) {
    if (!engine) return 0;
    return engine->engine.isActive() ? 1 : 0;
}

/* ========== Scanning ========== */

char* naab_gov_scan(naab_gov_engine_t engine,
                     const char* language,
                     const char* code,
                     const char* source_file,
                     int start_line) {
    if (!engine || !language || !code) return nullptr;

    try {
        CurrentEngineGuard guard(&engine->engine);

        engine->engine.resetCheckResults();
        std::string err = engine->engine.checkPolyglotBlock(
            language, code, source_file ? source_file : "", start_line);

        nlohmann::json result;
        result["blocked"] = engine->engine.wasBlocked();
        result["error"] = err;

        /* Parse the JSON report into the result so consumers get structured data */
        std::string json_report = engine->engine.generateJsonReport();
        if (!json_report.empty()) {
            try {
                result["report"] = nlohmann::json::parse(json_report);
            } catch (...) {
                result["report"] = json_report;  /* fallback: raw string */
            }
        } else {
            result["report"] = nlohmann::json::object();
        }

        engine->last_error.clear();
        return strdup(result.dump().c_str());
    } catch (const naab::governance::GovernanceHardError& e) {
        // HARD governance violations throw GovernanceHardError.
        // The C API must still return a valid result with blocked=true
        // rather than nullptr, so binding tests can check WasBlocked.
        nlohmann::json result;
        result["blocked"] = true;
        result["error"] = std::string(e.what());
        result["report"] = nlohmann::json::object();
        engine->last_error.clear();
        return strdup(result.dump().c_str());
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return nullptr;
    }
}

/* --- Single check dispatch --- */

using CheckFn = std::function<std::string(GovernanceEngine&,
                                           const std::string& /*lang*/,
                                           const std::string& /*code*/,
                                           int /*line*/)>;

static const std::unordered_map<std::string, CheckFn>& getCheckTable() {
    static const std::unordered_map<std::string, CheckFn> table = {
        {"secrets",
         [](auto& e, auto&, auto& c, int ln) { return e.checkSecrets(c, ln); }},
        {"code_injection",
         [](auto& e, auto& l, auto& c, int ln) { return e.checkCodeInjection(l, c, c, ln); }},
        {"sql_injection",
         [](auto& e, auto&, auto& c, int ln) { return e.checkSqlInjection(c, ln); }},
        {"dangerous_calls",
         [](auto& e, auto& l, auto& c, int ln) { return e.checkDangerousCall(l, c, ln); }},
        {"shell_injection",
         [](auto& e, auto&, auto& c, int ln) { return e.checkShellInjection(c, ln); }},
        {"imports",
         [](auto& e, auto& l, auto& c, int ln) { return e.checkImports(l, c, ln); }},
        {"obfuscation",
         [](auto& e, auto& l, auto& c, int ln) {
             /* obfuscation needs raw + stripped; use code as both for single-check mode */
             return e.checkObfuscationSignals(l, c, c, ln);
         }},
        {"deserialization",
         [](auto& e, auto&, auto& c, int ln) { return e.checkUnsafeDeserialization(c, ln); }},
        {"privilege_escalation",
         [](auto& e, auto&, auto& c, int ln) { return e.checkPrivilegeEscalation(c, ln); }},
        {"oversimplification",
         [](auto& e, auto&, auto& c, int ln) { return e.checkOversimplification(c, ln); }},
        {"incomplete_logic",
         [](auto& e, auto&, auto& c, int ln) { return e.checkIncompleteLogic(c, ln); }},
        {"encoding",
         [](auto& e, auto&, auto& c, int ln) { return e.checkEncoding(c, ln); }},
        {"path_traversal",
         [](auto& e, auto&, auto& c, int ln) { return e.checkPathTraversal(c, ln); }},
        {"data_exfiltration",
         [](auto& e, auto&, auto& c, int ln) { return e.checkDataExfiltration(c, ln); }},
        {"crypto_weakness",
         [](auto& e, auto&, auto& c, int ln) { return e.checkCryptoWeakness(c, c, ln); }},
    };
    return table;
}

char* naab_gov_check(naab_gov_engine_t engine,
                      const char* check_name,
                      const char* language,
                      const char* code,
                      int start_line) {
    if (!engine || !check_name || !language || !code) return nullptr;

    try {
        CurrentEngineGuard guard(&engine->engine);

        const auto& table = getCheckTable();
        auto it = table.find(check_name);
        if (it == table.end()) {
            engine->last_error = std::string("Unknown check: ") + check_name;
            return nullptr;
        }

        engine->engine.resetCheckResults();
        // C2 fix: preprocess code the same way checkPolyglotBlock does
        // (normalizeUnicode, normalizeWhitespace, stripStringLiterals,
        // expandDangerousAliases) so single-check API can't be bypassed
        // with homoglyphs, zero-width chars, or aliased functions
        std::string preprocessed = engine->engine.preprocessCode(
            std::string(language), std::string(code));
        std::string err = it->second(engine->engine,
                                      std::string(language),
                                      preprocessed,
                                      start_line);

        nlohmann::json result;
        result["check"] = check_name;
        result["blocked"] = engine->engine.wasBlocked();
        result["error"] = err;

        /* Include individual check results */
        const auto& results = engine->engine.getCheckResults();
        nlohmann::json results_arr = nlohmann::json::array();
        for (const auto& cr : results) {
            nlohmann::json r;
            r["rule_name"] = cr.rule_name;
            r["passed"] = cr.passed;
            r["message"] = cr.message;
            r["level"] = static_cast<int>(cr.level);
            if (!cr.cwe_ids.empty()) {
                r["cwe_ids"] = cr.cwe_ids;
            }
            if (!cr.owasp_ids.empty()) {
                r["owasp_ids"] = cr.owasp_ids;
            }
            if (!cr.rationale.empty()) {
                r["rationale"] = cr.rationale;
            }
            if (!cr.decision_trace.empty()) {
                r["decision_trace"] = cr.decision_trace;
            }
            results_arr.push_back(std::move(r));
        }
        result["results"] = std::move(results_arr);

        engine->last_error.clear();
        return strdup(result.dump().c_str());
    } catch (const naab::governance::GovernanceHardError& e) {
        nlohmann::json result;
        result["check"] = check_name;
        result["blocked"] = true;
        result["error"] = std::string(e.what());
        result["results"] = nlohmann::json::array();
        engine->last_error.clear();
        return strdup(result.dump().c_str());
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return nullptr;
    }
}

/* ========== Results ========== */

int naab_gov_was_blocked(naab_gov_engine_t engine) {
    if (!engine) return 0;
    return engine->engine.wasBlocked() ? 1 : 0;
}

char* naab_gov_json_report(naab_gov_engine_t engine) {
    if (!engine) return nullptr;
    try {
        std::string report = engine->engine.generateJsonReport();
        return strdup(report.c_str());
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return nullptr;
    }
}

char* naab_gov_sarif_report(naab_gov_engine_t engine) {
    if (!engine) return nullptr;
    try {
        std::string report = engine->engine.generateSarifReport();
        return strdup(report.c_str());
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return nullptr;
    }
}

char* naab_gov_summary(naab_gov_engine_t engine) {
    if (!engine) return nullptr;
    try {
        std::string summary = engine->engine.formatSummary();
        return strdup(summary.c_str());
    } catch (const std::exception& e) {
        engine->last_error = e.what();
        return nullptr;
    }
}

int naab_gov_result_count(naab_gov_engine_t engine) {
    if (!engine) return 0;
    return static_cast<int>(engine->engine.getCheckResults().size());
}

void naab_gov_reset(naab_gov_engine_t engine) {
    if (engine) {
        engine->engine.resetCheckResults();
    }
}

/* ========== Error handling ========== */

const char* naab_gov_last_error(naab_gov_engine_t engine) {
    if (!engine) return "";
    return engine->last_error.c_str();
}

/* ========== Memory ========== */

void naab_gov_free_string(char* str) {
    free(str);  /* NULL-safe per C spec */
}

/* ========== Version ========== */

const char* naab_gov_version_string(void) {
    return NAAB_GOV_VERSION;
}
