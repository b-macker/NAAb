// NAAb Agent Review — LLM-based governance review phase
// Detection agents analyze script source, validation agent filters false positives,
// findings score into enforcement tiers with agent-generated messages.

#pragma once
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
    int raw_count = 0;
    int confirmed_count = 0;
    int false_positive_count = 0;
    std::string zone;
    int score = 0;
    std::string voice_summary;  // synthesized remediation guide from voice agent
    std::string error;
};

// Run agent-based review of script source.
// Returns findings with agent-generated messages for use in enforce().
AgentReviewResult runAgentReview(
    const AgentReviewConfig& config,
    const governance::GovernanceRules& rules,
    const std::string& script_source,
    const std::string& govern_dir);

} // namespace runtime
} // namespace naab
