#include "naab/stdlib_new_modules.h"
#include "naab/score_accumulator.h"
#include "naab/governance.h"
#include "naab/finding_parser.h"

#include <mutex>
#include <memory>
#include <sstream>
#include <unordered_map>

using naab::interpreter::NaabVal;
using naab::governance::ScoreAccumulator;
using naab::governance::ScoringConfig;
using naab::governance::GovernanceEngine;

namespace naab {
namespace stdlib {

// ============================================================================
// Server-side state — immune to script handle mutation
// Same pattern as agent_impl.cpp:40-50
// ============================================================================

static int s_scorer_counter = 0;
static std::unordered_map<int, std::unique_ptr<ScoreAccumulator>> s_scorers;
// Each scorer holds a copy of ScoringConfig (not a reference to govern.json)
// so that config lifetime is safe even if the engine is destroyed.
static std::unordered_map<int, ScoringConfig> s_scorer_configs;
static std::mutex s_scorer_mutex;

// ============================================================================
// Helper: Build ScoringConfig from a named ScorerConfig or fall back to global
// ============================================================================

static ScoringConfig resolveScoringConfig(const std::string& config_name) {
    auto* engine = GovernanceEngine::getCurrent();
    if (!engine) {
        throw std::runtime_error(
            "Governance error: governance.scorer() requires an active governance engine\n\n"
            "  Help:\n"
            "  - Ensure govern.json is present in the project directory\n"
            "  - Ensure governance mode is not disabled\n");
    }

    const auto& rules = engine->getRules();

    // Look for a named scorer config
    for (const auto& sc : rules.scorers) {
        if (sc.name == config_name && sc.enabled) {
            ScoringConfig cfg;
            cfg.enabled = true;
            cfg.default_weight = sc.default_weight;
            cfg.rule_weights = sc.rule_weights;
            cfg.green_threshold = sc.green_threshold;
            cfg.yellow_threshold = sc.yellow_threshold;
            cfg.red_threshold = sc.red_threshold;
            cfg.threshold_mode = sc.threshold_mode;
            return cfg;
        }
    }

    // Fall back to global scoring config
    if (rules.scoring.enabled) {
        return rules.scoring;
    }

    // Last resort: return a default config
    ScoringConfig cfg;
    cfg.enabled = true;
    return cfg;
}

// ============================================================================
// governance.scorer(config_name) -> int
// ============================================================================

static NaabVal governanceScorer(std::vector<NaabVal>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error(
            "Type error: governance.scorer() expects a string config name\n\n"
            "  Got: " + std::string(args.empty() ? "no arguments" : "non-string") + "\n"
            "  Expected: string\n\n"
            "  Help:\n"
            "  - Pass the name of a scorer config from govern.json\n\n"
            "  Example:\n"
            "    x Wrong: governance.scorer(42)\n"
            "    v Right: governance.scorer(\"security_review\")\n");
    }

    std::string config_name = args[0].asString();
    ScoringConfig cfg = resolveScoringConfig(config_name);

    std::lock_guard<std::mutex> lock(s_scorer_mutex);
    int handle = ++s_scorer_counter;
    s_scorer_configs[handle] = cfg;
    s_scorers[handle] = std::make_unique<ScoreAccumulator>(s_scorer_configs[handle]);
    return NaabVal::makeInt(handle);
}

// ============================================================================
// governance.finding(handle, rule_name, message [, source]) -> dict
// ============================================================================

static NaabVal governanceFinding(std::vector<NaabVal>& args) {
    if (args.size() < 3 || !args[0].isInt() || !args[1].isString() || !args[2].isString()) {
        throw std::runtime_error(
            "Type error: governance.finding() expects (int handle, string rule, string message [, string source])\n\n"
            "  Help:\n"
            "  - handle: from governance.scorer()\n"
            "  - rule: rule name like \"sec.injection\"\n"
            "  - message: description of the finding\n"
            "  - source: (optional) which agent submitted this finding\n\n"
            "  Example:\n"
            "    v Right: governance.finding(h, \"sec.xss\", \"Unescaped input\", \"redteam\")\n");
    }

    int handle = static_cast<int>(args[0].asInt());
    std::string rule_name = args[1].asString();
    std::string message = args[2].asString();

    // Optional 4th arg: source
    std::string source;
    if (args.size() >= 4 && args[3].isString()) {
        source = args[3].asString();
    }

    ScoreAccumulator* acc = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_scorer_mutex);
        auto it = s_scorers.find(handle);
        if (it == s_scorers.end()) {
            throw std::runtime_error(
                "Governance error: Invalid scorer handle " + std::to_string(handle) + "\n\n"
                "  Help:\n"
                "  - Obtain a handle from governance.scorer() first\n");
        }
        acc = it->second.get();
    }

    int weight = acc->addFinding(rule_name, message, source);
    int score = acc->score();
    std::string zone = acc->zone();

    std::unordered_map<std::string, NaabVal> dict;
    dict["weight"] = NaabVal::makeInt(weight);
    dict["score"] = NaabVal::makeInt(score);
    dict["zone"] = NaabVal::makeString(zone);
    return NaabVal::makeDict(std::move(dict));
}

// ============================================================================
// governance.evaluate(handle) -> dict
// ============================================================================

static NaabVal governanceEvaluate(std::vector<NaabVal>& args) {
    if (args.empty() || !args[0].isInt()) {
        throw std::runtime_error(
            "Type error: governance.evaluate() expects an int handle\n\n"
            "  Help:\n"
            "  - Pass a handle from governance.scorer()\n");
    }

    int handle = static_cast<int>(args[0].asInt());

    ScoreAccumulator* acc = nullptr;
    ScoringConfig cfg;
    {
        std::lock_guard<std::mutex> lock(s_scorer_mutex);
        auto it = s_scorers.find(handle);
        if (it == s_scorers.end()) {
            throw std::runtime_error(
                "Governance error: Invalid scorer handle " + std::to_string(handle) + "\n\n"
                "  Help:\n"
                "  - Obtain a handle from governance.scorer() first\n");
        }
        acc = it->second.get();
        cfg = s_scorer_configs[handle];
    }

    int score = acc->score();
    std::string zone = acc->zone();
    std::string breakdown = acc->formatBreakdown();
    bool integrity = acc->verifyIntegrity();
    std::string hash = acc->integrityHash();
    const auto& findings = acc->findings();
    int src_count = acc->sourceCount();

    // Compute effective thresholds (scaled by source count in per_source mode)
    int scale = 1;
    if (cfg.threshold_mode == "per_source" && src_count > 0) {
        scale = src_count;
    }

    std::unordered_map<std::string, NaabVal> thresholds;
    thresholds["green"] = NaabVal::makeInt(cfg.green_threshold * scale);
    thresholds["yellow"] = NaabVal::makeInt(cfg.yellow_threshold * scale);
    thresholds["red"] = NaabVal::makeInt(cfg.red_threshold * scale);
    thresholds["mode"] = NaabVal::makeString(cfg.threshold_mode);
    thresholds["scale"] = NaabVal::makeInt(scale);

    std::unordered_map<std::string, NaabVal> dict;
    dict["score"] = NaabVal::makeInt(score);
    dict["zone"] = NaabVal::makeString(zone);
    dict["findings_count"] = NaabVal::makeInt(static_cast<int>(findings.size()));
    dict["source_count"] = NaabVal::makeInt(src_count);
    dict["breakdown"] = NaabVal::makeString(breakdown);
    dict["integrity_verified"] = NaabVal::makeBool(integrity);
    dict["integrity_hash"] = NaabVal::makeString(hash);
    dict["thresholds"] = NaabVal::makeDict(std::move(thresholds));
    return NaabVal::makeDict(std::move(dict));
}

// ============================================================================
// governance.findings(handle) -> array
// ============================================================================

static NaabVal governanceFindings(std::vector<NaabVal>& args) {
    if (args.empty() || !args[0].isInt()) {
        throw std::runtime_error(
            "Type error: governance.findings() expects an int handle\n\n"
            "  Help:\n"
            "  - Pass a handle from governance.scorer()\n");
    }

    int handle = static_cast<int>(args[0].asInt());

    ScoreAccumulator* acc = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_scorer_mutex);
        auto it = s_scorers.find(handle);
        if (it == s_scorers.end()) {
            throw std::runtime_error(
                "Governance error: Invalid scorer handle " + std::to_string(handle) + "\n\n"
                "  Help:\n"
                "  - Obtain a handle from governance.scorer() first\n");
        }
        acc = it->second.get();
    }

    std::vector<NaabVal> arr;
    for (const auto& f : acc->findings()) {
        std::unordered_map<std::string, NaabVal> entry;
        entry["rule"] = NaabVal::makeString(f.rule_name);
        entry["message"] = NaabVal::makeString(f.message);
        entry["source"] = NaabVal::makeString(f.source);
        entry["weight"] = NaabVal::makeInt(f.weight);
        entry["score_after"] = NaabVal::makeInt(f.score_after);
        arr.push_back(NaabVal::makeDict(std::move(entry)));
    }
    return NaabVal::makeList(std::move(arr));
}

// ============================================================================
// governance.score(handle) -> int
// ============================================================================

static NaabVal governanceScore(std::vector<NaabVal>& args) {
    if (args.empty() || !args[0].isInt()) {
        throw std::runtime_error(
            "Type error: governance.score() expects an int handle\n\n"
            "  Help:\n"
            "  - Pass a handle from governance.scorer()\n");
    }

    int handle = static_cast<int>(args[0].asInt());

    ScoreAccumulator* acc = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_scorer_mutex);
        auto it = s_scorers.find(handle);
        if (it == s_scorers.end()) {
            throw std::runtime_error(
                "Governance error: Invalid scorer handle " + std::to_string(handle) + "\n\n"
                "  Help:\n"
                "  - Obtain a handle from governance.scorer() first\n");
        }
        acc = it->second.get();
    }

    return NaabVal::makeInt(acc->score());
}

// ============================================================================
// governance.parse_findings(text) -> list of {category, description}
// Robust parser for FINDING|category|description lines from agent output.
// Handles markdown code blocks, preamble, and delimiter variations (|, :, -)
// ============================================================================

static NaabVal governanceParseFindings(std::vector<NaabVal>& args) {
    if (args.empty() || !args[0].isString()) {
        throw std::runtime_error(
            "Type error: governance.parse_findings() expects a string\n\n"
            "  Help:\n"
            "  - Pass raw agent output text\n"
            "  - Returns list of {\"category\": ..., \"description\": ...} dicts\n\n"
            "  Example:\n"
            "    v Right: governance.parse_findings(agent_response)\n");
    }

    // Delegate to shared parser
    auto parsed = runtime::parseFindings(args[0].asString());

    std::vector<NaabVal> results;
    for (const auto& f : parsed) {
        std::unordered_map<std::string, NaabVal> entry;
        entry["category"] = NaabVal::makeString(f.category);
        entry["description"] = NaabVal::makeString(f.description);
        results.push_back(NaabVal::makeDict(std::move(entry)));
    }
    return NaabVal::makeList(std::move(results));
}

// ============================================================================
// governance.health() — pulse verdict + liveness counters
// ============================================================================

static NaabVal governanceHealth(std::vector<NaabVal>& /*args*/) {
    auto* engine = GovernanceEngine::getCurrent();
    if (!engine) {
        // No governance active — return healthy (no false positives)
        std::unordered_map<std::string, NaabVal> result;
        result["verdict"] = NaabVal::makeString("healthy");
        result["active"] = NaabVal::makeBool(false);
        return NaabVal::makeDict(std::move(result));
    }

    auto pulse = engine->getPulse();
    std::unordered_map<std::string, NaabVal> result;

    const char* verdict_str = "healthy";
    if (pulse.verdict == governance::PulseVerdict::DEGRADED) verdict_str = "degraded";
    else if (pulse.verdict == governance::PulseVerdict::IMPAIRED) verdict_str = "impaired";

    result["verdict"] = NaabVal::makeString(verdict_str);
    result["active"] = NaabVal::makeBool(true);
    result["total_checks"] = NaabVal::makeInt(pulse.total_checks);
    result["consecutive_passes"] = NaabVal::makeInt(pulse.consecutive_passes);
    result["refusal_count"] = NaabVal::makeInt(pulse.refusal_count);
    // Full breakdown for scripts — operator context, not agent context
    result["bsd_connected"] = NaabVal::makeBool(pulse.bsd_connected);
    result["cdd_connected"] = NaabVal::makeBool(pulse.cdd_connected);
    result["telemetry_connected"] = NaabVal::makeBool(pulse.telemetry_connected);
    // Evidence epoch — monotonic state transition counter
    result["governance_epoch"] = NaabVal::makeInt(engine->getGovernanceEpoch());
    return NaabVal::makeDict(std::move(result));
}

// ============================================================================
// Module interface
// ============================================================================

bool GovernanceModule::hasFunction(const std::string& name) const {
    return name == "scorer" || name == "finding" || name == "evaluate" ||
           name == "findings" || name == "score" || name == "parse_findings" ||
           name == "health";
}

NaabVal GovernanceModule::call(
    const std::string& function_name,
    std::vector<NaabVal>& args) {
    if (function_name == "scorer") return governanceScorer(args);
    if (function_name == "finding") return governanceFinding(args);
    if (function_name == "evaluate") return governanceEvaluate(args);
    if (function_name == "findings") return governanceFindings(args);
    if (function_name == "score") return governanceScore(args);
    if (function_name == "parse_findings") return governanceParseFindings(args);
    if (function_name == "health") return governanceHealth(args);

    throw std::runtime_error(
        "Runtime error: governance module has no function '" + function_name + "'\n\n"
        "  Help:\n"
        "  - Available: scorer, finding, evaluate, findings, score, parse_findings, health\n");
}

} // namespace stdlib
} // namespace naab
