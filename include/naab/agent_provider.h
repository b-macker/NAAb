// NAAb Agent Provider — Shared LLM API calling layer
// Used by both stdlib/agent_impl.cpp and runtime/agent_review.cpp
// No nlohmann/json in this header (project convention)

#pragma once
#include "naab/governance.h"
#include <string>

namespace naab {
namespace runtime {

// Normalized response from any LLM provider
struct AgentResponse {
    bool success = false;
    std::string content;
    std::string stop_reason;
    int input_tokens = 0;
    int output_tokens = 0;
    std::string error;  // non-empty on failure
};

// Resolve API key: checks env var first, then falls back to ~/.naab/keys/<varname>.
// Returns empty string if neither source has the key.
std::string resolveApiKey(const std::string& env_var_name);

// Single-shot: send one user prompt, get normalized response. Stateless.
// Uses config.system_prompt if non-empty.
AgentResponse callAgentSimple(
    const governance::AgentConfig& config,
    const std::string& api_key,
    const std::string& user_prompt);

// Multi-turn: send conversation as JSON string (Anthropic-style messages array).
// Returns normalized response (same as single-shot).
AgentResponse callAgentMultiTurn(
    const governance::AgentConfig& config,
    const std::string& api_key,
    const std::string& messages_json);

} // namespace runtime
} // namespace naab
