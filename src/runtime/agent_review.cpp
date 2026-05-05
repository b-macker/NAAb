// NAAb Agent Review — LLM-based governance review phase
// Orchestrates: detection agents → validation agent → scoring → enforcement mapping

#include "naab/agent_review.h"
#include "naab/agent_provider.h"
#include "naab/finding_parser.h"
#include "naab/score_accumulator.h"
#include "naab/governance.h"
#include "naab/crypto_utils.h"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

namespace naab {
namespace runtime {

using json = nlohmann::json;
using governance::AgentConfig;
using governance::ScoringConfig;
using governance::ScoreAccumulator;

// ============================================================================
// Helper: Find agent config by name in rules
// ============================================================================

static const AgentConfig* findAgent(const governance::GovernanceRules& rules,
                                     const std::string& name) {
    for (const auto& agent : rules.agents) {
        if (agent.name == name) return &agent;
    }
    return nullptr;
}

// ============================================================================
// Helper: Resolve scorer config by name
// ============================================================================

static ScoringConfig resolveScorerConfig(const governance::GovernanceRules& rules,
                                          const std::string& name) {
    for (const auto& sc : rules.scorers) {
        if (sc.name == name && sc.enabled) {
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
    // Fallback to global scoring config
    if (rules.scoring.enabled) {
        return rules.scoring;
    }
    ScoringConfig cfg;
    cfg.enabled = true;
    return cfg;
}

// ============================================================================
// Helper: Build category list from scorer rule_weights
// ============================================================================

static std::string buildCategoryList(const ScoringConfig& cfg) {
    std::string cats;
    for (const auto& [key, _] : cfg.rule_weights) {
        if (!cats.empty()) cats += ", ";
        cats += key;
    }
    return cats;
}

// ============================================================================
// Helper: Build detection prompt
// ============================================================================

static std::string buildDetectionPrompt(const std::string& categories,
                                         const std::string& source) {
    std::string prompt;
    prompt += "You are a code reviewer. Analyze this script for issues. ";
    prompt += "For each issue found, output EXACTLY one line:\n";
    prompt += "FINDING|<category>|<description>\n";
    if (!categories.empty()) {
        prompt += "Categories: " + categories + "\n";
    }
    prompt += "Do NOT include any literal secret values or API keys.\n\n";
    prompt += "Script to analyze:\n" + source;
    return prompt;
}

// ============================================================================
// Helper: Build validation prompt
// ============================================================================

static std::string buildValidationPrompt(const std::string& findings_text,
                                          const std::string& source) {
    std::string prompt;
    prompt += "You are a senior analyst. Review these findings about a script. ";
    prompt += "For each finding, determine if it is a real issue or a false positive. ";
    prompt += "Output one line per finding:\n";
    prompt += "VERDICT|CONFIRMED|<category>|<reason>  or  VERDICT|FALSE_POSITIVE|<category>|<reason>\n";
    prompt += "Do NOT include any literal secret values or API keys.\n\n";
    prompt += "Findings:\n" + findings_text;
    prompt += "\n\nScript:\n" + source;
    return prompt;
}

// ============================================================================
// Helper: Cache path
// ============================================================================

static std::string cachePath(const std::string& govern_dir, const std::string& hash) {
    return govern_dir + "/.naab_cache/" + hash + ".review.json";
}

static bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static void ensureDir(const std::string& path) {
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
}

// ============================================================================
// Helper: Load cached result
// ============================================================================

static bool loadCache(const std::string& path, AgentReviewResult& result) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    try {
        json j;
        f >> j;
        result.executed = true;
        result.cache_hit = true;
        result.source_hash = j.value("source_hash", "");
        result.raw_count = j.value("raw_count", 0);
        result.confirmed_count = j.value("confirmed_count", 0);
        result.false_positive_count = j.value("false_positive_count", 0);
        result.zone = j.value("zone", "green");
        result.score = j.value("score", 0);
        result.voice_summary = j.value("voice_summary", "");

        if (j.contains("findings") && j["findings"].is_array()) {
            for (const auto& fj : j["findings"]) {
                AgentReviewFinding finding;
                finding.category = fj.value("category", "");
                finding.message = fj.value("message", "");
                finding.source_agent = fj.value("source_agent", "");
                finding.validated = fj.value("validated", false);
                finding.confidence = fj.value("confidence", 1.0);
                result.findings.push_back(finding);
            }
        }
        if (j.contains("rejected") && j["rejected"].is_array()) {
            for (const auto& fj : j["rejected"]) {
                AgentReviewFinding finding;
                finding.category = fj.value("category", "");
                finding.message = fj.value("message", "");
                finding.source_agent = fj.value("source_agent", "");
                finding.validated = false;
                result.rejected_findings.push_back(finding);
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Helper: Save cache
// ============================================================================

static void saveCache(const std::string& path, const AgentReviewResult& result) {
    json j;
    j["source_hash"] = result.source_hash;
    j["raw_count"] = result.raw_count;
    j["confirmed_count"] = result.confirmed_count;
    j["false_positive_count"] = result.false_positive_count;
    j["zone"] = result.zone;
    j["score"] = result.score;
    j["voice_summary"] = result.voice_summary;

    json findings_arr = json::array();
    for (const auto& f : result.findings) {
        json fj;
        fj["category"] = f.category;
        fj["message"] = f.message;
        fj["source_agent"] = f.source_agent;
        fj["validated"] = f.validated;
        fj["confidence"] = f.confidence;
        findings_arr.push_back(fj);
    }
    j["findings"] = findings_arr;

    json rejected_arr = json::array();
    for (const auto& f : result.rejected_findings) {
        json fj;
        fj["category"] = f.category;
        fj["message"] = f.message;
        fj["source_agent"] = f.source_agent;
        rejected_arr.push_back(fj);
    }
    j["rejected"] = rejected_arr;

    std::ofstream out(path);
    if (out.is_open()) {
        out << j.dump(2);
    }
}

// ============================================================================
// Main entry point
// ============================================================================

AgentReviewResult runAgentReview(
    const AgentReviewConfig& config,
    const governance::GovernanceRules& rules,
    const std::string& script_source,
    const std::string& govern_dir) {

    AgentReviewResult result;

    if (!config.enabled) {
        return result;
    }

    // Compute source hash
    result.source_hash = security::CryptoUtils::sha256(script_source);

    // Cache check
    if (config.cache && !govern_dir.empty()) {
        std::string cpath = cachePath(govern_dir, result.source_hash);
        if (fileExists(cpath)) {
            AgentReviewResult cached;
            if (loadCache(cpath, cached)) {
                return cached;
            }
        }
    }

    result.executed = true;

    // Resolve scorer config for category list and scoring
    ScoringConfig scorer_cfg = resolveScorerConfig(rules, config.scorer_name);
    std::string categories = buildCategoryList(scorer_cfg);

    // ── Detection phase ──
    std::vector<ParsedFinding> all_raw_findings;
    std::vector<std::string> finding_sources;  // parallel to all_raw_findings
    std::string all_findings_text;  // for validation prompt

    for (const auto& agent_name : config.detection_agents) {
        const AgentConfig* acfg = findAgent(rules, agent_name);
        if (!acfg) {
            result.error = "Agent review: detection agent '" + agent_name + "' not found in govern.json";
            return result;
        }

        const char* api_key = std::getenv(acfg->api_key_env.c_str());
        if (!api_key || std::string(api_key).empty()) {
            result.error = "Agent review: API key env var '" + acfg->api_key_env + "' not set for agent '" + agent_name + "'";
            return result;
        }

        std::string prompt = buildDetectionPrompt(categories, script_source);
        auto resp = callAgentSimple(*acfg, std::string(api_key), prompt);

        if (!resp.success) {
            result.error = "Agent review: detection agent '" + agent_name + "' failed: " + resp.error;
            return result;
        }

        auto parsed = parseFindings(resp.content);

        for (const auto& f : parsed) {
            all_raw_findings.push_back(f);
            finding_sources.push_back(agent_name);
            all_findings_text += "FINDING|" + f.category + "|" + f.description + "\n";
        }
    }

    result.raw_count = static_cast<int>(all_raw_findings.size());

    if (all_raw_findings.empty()) {
        result.zone = "green";
        result.score = 0;
        // Cache and return
        if (config.cache && !govern_dir.empty()) {
            ensureDir(govern_dir + "/.naab_cache");
            saveCache(cachePath(govern_dir, result.source_hash), result);
        }
        return result;
    }

    // ── Validation phase ──
    if (!config.validation_agent.empty()) {
        const AgentConfig* val_cfg = findAgent(rules, config.validation_agent);
        if (!val_cfg) {
            result.error = "Agent review: validation agent '" + config.validation_agent + "' not found in govern.json";
            return result;
        }

        const char* val_key = std::getenv(val_cfg->api_key_env.c_str());
        if (!val_key || std::string(val_key).empty()) {
            result.error = "Agent review: API key not set for validation agent '" + config.validation_agent + "'";
            return result;
        }

        std::string val_prompt = buildValidationPrompt(all_findings_text, script_source);
        auto val_resp = callAgentSimple(*val_cfg, std::string(val_key), val_prompt);

        if (!val_resp.success) {
            result.error = "Agent review: validation agent failed: " + val_resp.error;
            return result;
        }

        auto verdicts = parseVerdicts(val_resp.content);

        // Build set of confirmed categories
        std::vector<std::string> confirmed_cats;
        for (const auto& v : verdicts) {
            if (v.confirmed) {
                confirmed_cats.push_back(v.category);
            }
        }

        // Filter findings: keep only confirmed
        for (size_t i = 0; i < all_raw_findings.size(); i++) {
            bool is_confirmed = false;
            for (const auto& cat : confirmed_cats) {
                if (cat == all_raw_findings[i].category) {
                    is_confirmed = true;
                    break;
                }
            }
            if (is_confirmed) {
                AgentReviewFinding f;
                f.category = all_raw_findings[i].category;
                f.message = all_raw_findings[i].description;
                f.source_agent = finding_sources[i];
                f.validated = true;
                result.findings.push_back(f);
                result.confirmed_count++;
            } else {
                result.false_positive_count++;
                AgentReviewFinding rf;
                rf.category = all_raw_findings[i].category;
                rf.message = all_raw_findings[i].description;
                rf.source_agent = finding_sources[i];
                rf.validated = false;
                result.rejected_findings.push_back(rf);
            }
        }

    } else {
        // No validation agent — all findings pass through
        for (size_t i = 0; i < all_raw_findings.size(); i++) {
            AgentReviewFinding f;
            f.category = all_raw_findings[i].category;
            f.message = all_raw_findings[i].description;
            f.source_agent = finding_sources[i];
            f.validated = false;
            result.findings.push_back(f);
            result.confirmed_count++;
        }
    }

    // ── Deduplication phase ──
    // Collapse findings by category — keep first message per category, note agent agreement
    std::map<std::string, AgentReviewFinding> deduped;
    std::map<std::string, std::vector<std::string>> category_agents;
    for (const auto& f : result.findings) {
        if (deduped.find(f.category) == deduped.end()) {
            deduped[f.category] = f;
        }
        category_agents[f.category].push_back(f.source_agent);
    }
    // Replace findings with deduplicated set, compute consensus confidence
    int total_agents = static_cast<int>(config.detection_agents.size());
    std::vector<AgentReviewFinding> deduped_findings;
    for (auto& [cat, f] : deduped) {
        auto& agents = category_agents[cat];
        std::set<std::string> unique_agents(agents.begin(), agents.end());

        // Consensus confidence: agents_found / total_agents
        if (total_agents > 0) {
            f.confidence = static_cast<double>(unique_agents.size()) / total_agents;
        }

        if (unique_agents.size() > 1) {
            // Multiple agents agreed — note consensus
            std::string agent_list;
            for (const auto& a : unique_agents) {
                if (!agent_list.empty()) agent_list += ", ";
                agent_list += a;
            }
            f.source_agent = agent_list;
        }
        deduped_findings.push_back(f);
    }
    result.findings = deduped_findings;

    // ── Scoring phase ──
    ScoreAccumulator scorer(scorer_cfg);
    for (const auto& f : result.findings) {
        scorer.addFinding(f.category, f.message, f.source_agent, f.confidence);
    }

    result.zone = scorer.zone();
    result.score = scorer.score();

    // ── Voice phase ──
    // Synthesize all findings into one actionable remediation guide
    if (!config.voice_agent.empty() && !result.findings.empty()) {
        const AgentConfig* voice_cfg = findAgent(rules, config.voice_agent);
        if (voice_cfg) {
            const char* voice_key = std::getenv(voice_cfg->api_key_env.c_str());
            if (voice_key && std::string(voice_key).length() > 0) {
                // Build finding summary for voice prompt
                std::string finding_list;
                int num = 1;
                for (const auto& f : result.findings) {
                    finding_list += std::to_string(num++) + ". [" + f.category + "] " + f.message;
                    if (f.confidence < 1.0) {
                        int pct = static_cast<int>(f.confidence * 100 + 0.5);
                        finding_list += " (confidence: " + std::to_string(pct) + "%)";
                    }
                    finding_list += "\n";
                }

                std::string voice_prompt;
                voice_prompt += "You are a governance advisor. A code review found these issues in a script.\n";
                voice_prompt += "Zone: " + result.zone + " (score: " + std::to_string(result.score) + ")\n\n";
                voice_prompt += "Findings:\n" + finding_list + "\n";
                voice_prompt += "Script:\n" + script_source + "\n\n";
                voice_prompt += "Write a SHORT, actionable remediation guide. For each issue:\n";
                voice_prompt += "- Reference specific line numbers or function names from the script\n";
                voice_prompt += "- Say exactly what to change (not vague advice)\n";
                voice_prompt += "- One or two sentences per issue, max\n";
                voice_prompt += "Number each fix. No preamble, no summary paragraph. Just the numbered fixes.\n";
                voice_prompt += "Do NOT include any literal secret values, API keys, or credentials.\n";

                auto voice_resp = callAgentSimple(*voice_cfg, std::string(voice_key), voice_prompt);
                if (voice_resp.success && !voice_resp.content.empty()) {
                    result.voice_summary = voice_resp.content;
                }
            }
        }
    }

    // ── Cache write ──
    if (config.cache && !govern_dir.empty()) {
        ensureDir(govern_dir + "/.naab_cache");
        saveCache(cachePath(govern_dir, result.source_hash), result);
    }

    return result;
}

} // namespace runtime
} // namespace naab
