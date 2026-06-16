// NAAb Agent Provider — Shared LLM API calling layer
// Used by both stdlib/agent_impl.cpp and runtime/agent_review.cpp
// No nlohmann/json in this header (project convention)

#pragma once
#include "naab/governance.h"
#include <string>

namespace naab {
namespace runtime {

// Tool call requested by LLM (parsed from tool_use / functionCall responses)
struct ToolCallInfo {
    std::string id;        // tool_use block id (Anthropic) or generated id (Gemini)
    std::string name;      // function name requested by LLM
    std::string arguments; // JSON string of arguments
};

// Normalized response from any LLM provider
struct AgentResponse {
    bool success = false;
    std::string content;
    std::string stop_reason;
    int input_tokens = 0;
    int output_tokens = 0;
    bool truncated = false;      // true when response hit token limit
    int thinking_tokens = 0;     // thinking tokens consumed (Gemini thoughtsTokenCount)
    std::string error;  // non-empty on failure

    // Tool calls (populated when stop_reason indicates tool use)
    std::vector<ToolCallInfo> tool_calls;

    // Traceability (populated by retry loop in agent_impl, not by provider)
    int http_status = 0;            // last HTTP status code
    std::string actual_model;       // model that actually responded
    std::string actual_provider;    // provider used
    std::string actual_api_key_env; // env var name of key used
    int64_t latency_ms = 0;        // wall-clock time of successful call
    int attempts = 1;              // total attempts made
    bool fallback_used = false;    // true if non-primary model responded
    std::string original_model;    // primary model (if fallback_used)
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

// Multi-turn with HTTP status: same as callAgentMultiTurn but returns status code
// on failure instead of throwing. Used by retry loop in agent_impl.
struct ProviderResult {
    AgentResponse response;
    int http_status = 0;   // HTTP status code (0 = network/parse error)
};

ProviderResult callAgentWithStatus(
    const governance::AgentConfig& config,
    const std::string& api_key,
    const std::string& messages_json);

// Tool definition for sending to LLM providers
struct ToolDefinition {
    std::string name;
    std::string description;
    std::string input_schema_json;  // JSON string of parameter schema
};

// Multi-turn with tool definitions: sends tool schemas to LLM, parses tool_use responses
ProviderResult callAgentWithTools(
    const governance::AgentConfig& config,
    const std::string& api_key,
    const std::string& messages_json,
    const std::vector<ToolDefinition>& tools);

} // namespace runtime
} // namespace naab
