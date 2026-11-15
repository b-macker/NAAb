// NAAb Agent Review — LLM-based governance review phase
// Detection agents analyze script source, validation agent filters false positives,
// findings score into enforcement tiers with agent-generated messages.

#pragma once
#include <functional>
#include <string>
#include <vector>
#include <map>

namespace naab {

// Forward declarations to avoid heavy includes in header
namespace governance {
    struct GovernanceRules;
}

namespace runtime {

struct AgentReviewConfig {
    bool enabled = false;
    std::vector<std::string> detection_agents;
    std::string validation_agent;
    std::string voice_agent;         // synthesizes findings into one actionable message
    std::string scorer_name;
    std::map<std::string, std::string> enforcement;  // zone -> level (advisory/soft/hard)
    bool cache = false;
    bool hints = false;         // show rejected findings as [hint] lines
    std::string config_hash;    // govern.json SHA-256 — invalidates cache on config change
    std::string dispatch_mode = "sequential";  // "sequential" | "parallel"
    int max_parallel = 0;       // 0 = unlimited (all detection agents at once)
    std::string fail_strategy = "fail_fast";   // "fail_fast" | "continue"
};

struct AgentReviewFinding {
    std::string category;
    std::string message;        // agent-generated "living organic message"
    std::string source_agent;
    bool validated;
    double confidence = 1.0;    // consensus ratio: agents_found / total_agents
};

struct AgentReviewResult {
    bool executed = false;
    bool cache_hit = false;
    std::string source_hash;
    std::vector<AgentReviewFinding> findings;
    std::vector<AgentReviewFinding> rejected_findings;  // analyst-rejected (for hints)
    int raw_count = 0;
    int confirmed_count = 0;
    int false_positive_count = 0;
    std::string zone;
    int score = 0;
    std::string voice_summary;  // synthesized remediation guide from voice agent
    std::string voice_withheld; // why the guide was withheld ("secret" / "PII"), else ""
    int texts_withheld = 0;     // finding descriptions and categories withheld (A166)
    std::string error;
};

// Says why a text a reviewer model wrote must not be shown ("secret", "PII"),
// or "" when it may. Decide-only: it enforces and records nothing.
using ModelTextWithheld = std::function<std::string(const std::string&)>;

// Run agent-based review of script source.
// Returns findings with agent-generated messages for use in enforce().
// Every text a model wrote -- each finding's category and description, the
// rejected findings, the voice guide -- goes through `withheld` once, on the
// result returned (fresh or read from the cache) and before the cache is
// written; a text it names is replaced by a notice. The zone, score and level
// are computed first, so withholding never changes what the review enforces
// (A166).
AgentReviewResult runAgentReview(
    const AgentReviewConfig& config,
    const governance::GovernanceRules& rules,
    const std::string& script_source,
    const std::string& govern_dir,
    const ModelTextWithheld& withheld);

} // namespace runtime
} // namespace naab
