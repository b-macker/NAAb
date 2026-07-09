// orchestra_impl.cpp — Multi-agent coordination patterns implementation
// Provides sequential refinement, consensus voting, and convergence enforcement

#include "naab/orchestra.h"
#include "naab/naab_val.h"
#include "naab/interpreter.h"
#include "naab/governance.h"
#include <fmt/core.h>
#include <regex>
#include <nlohmann/json.hpp>

namespace naab {
namespace stdlib {

// ============================================================================
// Orchestra.sequential_refinement(handles, prompt, iterations=3)
// ============================================================================
// Helper function that returns a plan dict for sequential refinement.
// NAAb code implements the actual agent.send() loop.
// Returns {handles, prompt, iterations, description}

interpreter::NaabVal orchestraSequentialRefinement(std::vector<interpreter::NaabVal>& args) {
    if (args.size() < 2 || args.size() > 3) {
        throw std::runtime_error(
            "Orchestra error: orchestra.sequential_refinement() takes 2-3 arguments\n\n"
            "  Got: " + std::to_string(args.size()) + " argument(s)\n"
            "  Expected: orchestra.sequential_refinement(handles, prompt [, iterations])\n\n"
            "  Example:\n"
            "    let plan = orchestra.sequential_refinement([h1, h2], \"Analyze this\")\n"
            "    let plan = orchestra.sequential_refinement([h1, h2], \"Analyze\", 5)\n");
    }

    // Validate handles array
    if (!args[0].isList()) {
        throw std::runtime_error(
            "Orchestra error: first argument must be an array of agent handles\n\n"
            "  Got: " + args[0].getTypeName() + "\n"
            "  Expected: array of handles from agent.create()\n");
    }
    const auto& handles = args[0].asListConst();
    if (handles.empty()) {
        throw std::runtime_error(
            "Orchestra error: handles array cannot be empty\n\n"
            "  Expected: at least one agent handle\n");
    }

    // Validate prompt
    if (!args[1].isString()) {
        throw std::runtime_error(
            "Orchestra error: prompt must be a string\n\n"
            "  Got: " + args[1].getTypeName() + "\n");
    }
    std::string prompt = args[1].asString();

    // Parse iterations (default 3)
    int iterations = 3;
    if (args.size() == 3) {
        if (!args[2].isInt()) {
            throw std::runtime_error(
                "Orchestra error: iterations must be an integer\n\n"
                "  Got: " + args[2].getTypeName() + "\n");
        }
        iterations = args[2].asInt();
        if (iterations < 1) {
            throw std::runtime_error(
                "Orchestra error: iterations must be >= 1\n\n"
                "  Got: " + std::to_string(iterations) + "\n");
        }
    }

    // Return plan dict
    std::unordered_map<std::string, interpreter::NaabVal> plan;
    plan["pattern"] = interpreter::NaabVal::makeString("sequential_refinement");
    plan["handles"] = args[0];  // handles array
    plan["prompt"] = interpreter::NaabVal::makeString(prompt);
    plan["iterations"] = interpreter::NaabVal::makeInt(iterations);
    plan["description"] = interpreter::NaabVal::makeString(
        fmt::format("Sequential refinement across {} agents for {} iteration(s)",
                   handles.size(), iterations));

    return interpreter::NaabVal::makeDict(std::move(plan));
}

// ============================================================================
// Orchestra.consensus_vote(votes_dict)
// ============================================================================
// Helper function to tabulate and validate votes
// Input: {votes: ["APPROVED", "REJECTED", "APPROVED"]}
// Returns {verdict, votes, approved, rejected, review, majority}

interpreter::NaabVal orchestraConsensusVote(std::vector<interpreter::NaabVal>& args) {
    if (args.size() != 1) {
        throw std::runtime_error(
            "Orchestra error: orchestra.consensus_vote() takes exactly 1 argument\n\n"
            "  Got: " + std::to_string(args.size()) + " argument(s)\n"
            "  Expected: orchestra.consensus_vote(votes_dict)\n\n"
            "  Example:\n"
            "    let result = orchestra.consensus_vote({votes: [\"APPROVED\", \"APPROVED\", \"REJECTED\"]})\n");
    }

    // Validate input
    if (!args[0].isDict()) {
        throw std::runtime_error(
            "Orchestra error: argument must be a dict with 'votes' key\n");
    }

    auto input_dict = args[0].asDictConst();
    if (input_dict.find("votes") == input_dict.end()) {
        throw std::runtime_error(
            "Orchestra error: dict must contain 'votes' key (array of verdicts)\n");
    }

    if (!input_dict.at("votes").isList()) {
        throw std::runtime_error(
            "Orchestra error: 'votes' must be an array of verdict strings\n");
    }

    const auto& votes_list = input_dict.at("votes").asListConst();
    if (votes_list.empty()) {
        throw std::runtime_error(
            "Orchestra error: 'votes' array cannot be empty\n");
    }

    // Tally votes
    int approved_count = 0;
    int review_count = 0;
    int rejected_count = 0;

    for (const auto& vote_val : votes_list) {
        if (!vote_val.isString()) {
            throw std::runtime_error(
                "Orchestra error: each vote must be a string\n");
        }

        std::string vote = vote_val.asString();
        std::string upper_vote = vote;
        std::transform(upper_vote.begin(), upper_vote.end(),
                      upper_vote.begin(), ::toupper);

        if (upper_vote == "APPROVED") {
            approved_count++;
        } else if (upper_vote == "REJECTED") {
            rejected_count++;
        } else if (upper_vote == "REVIEW" || upper_vote == "NEEDS_WORK") {
            review_count++;
        } else {
            review_count++;
        }
    }

    // Determine majority verdict
    int total_votes = static_cast<int>(votes_list.size());
    int majority = total_votes / 2 + 1;
    std::string final_verdict = "REVIEW";

    if (approved_count >= majority) {
        final_verdict = "APPROVED";
    } else if (rejected_count >= majority) {
        final_verdict = "REJECTED";
    }

    // Build result dict
    std::unordered_map<std::string, interpreter::NaabVal> result_dict;
    result_dict["verdict"] = interpreter::NaabVal::makeString(final_verdict);
    result_dict["majority"] = interpreter::NaabVal::makeInt(majority);
    result_dict["approved"] = interpreter::NaabVal::makeInt(approved_count);
    result_dict["rejected"] = interpreter::NaabVal::makeInt(rejected_count);
    result_dict["review"] = interpreter::NaabVal::makeInt(review_count);
    result_dict["total"] = interpreter::NaabVal::makeInt(total_votes);

    return interpreter::NaabVal::makeDict(std::move(result_dict));
}

// ============================================================================
// Orchestra.enforce_convergence(response, spec)
// ============================================================================
// Validates response against spec (regex pattern or required JSON fields)
// Returns {valid, error_message} or throws on invalid spec

interpreter::NaabVal orchestraEnforceConvergence(std::vector<interpreter::NaabVal>& args) {
    if (args.size() != 2) {
        throw std::runtime_error(
            "Orchestra error: orchestra.enforce_convergence() takes exactly 2 arguments\n\n"
            "  Got: " + std::to_string(args.size()) + " argument(s)\n"
            "  Expected: orchestra.enforce_convergence(response, spec)\n\n"
            "  Example:\n"
            "    let result = orchestra.enforce_convergence(response, {pattern: \"^[A-Z]+$\"})\n");
    }

    // Validate response
    std::string response = args[0].toString();

    // Validate spec
    if (!args[1].isDict()) {
        throw std::runtime_error(
            "Orchestra error: spec must be a dict with pattern or required_fields\n");
    }
    auto spec = args[1].asDictConst();

    // Perform validation
    bool is_valid = false;
    std::string validation_error;

    // Check for regex pattern
    if (spec.find("pattern") != spec.end() && spec.at("pattern").isString()) {
        std::string pattern_str = spec.at("pattern").asString();
        try {
            std::regex pattern(pattern_str);
            if (std::regex_search(response, pattern)) {
                is_valid = true;
            } else {
                validation_error = fmt::format(
                    "response does not match pattern: {}", pattern_str);
            }
        } catch (const std::regex_error& e) {
            validation_error = fmt::format("invalid regex pattern: {}", e.what());
        }
    }

    // Check for required_fields (JSON contract-like)
    if (!is_valid && spec.find("required_fields") != spec.end() &&
        spec.at("required_fields").isList()) {
        // Try to parse as JSON first
        try {
            auto json_obj = nlohmann::json::parse(response);
            is_valid = true;
            for (const auto& field_val : spec.at("required_fields").asListConst()) {
                if (field_val.isString()) {
                    std::string field = field_val.asString();
                    if (!json_obj.contains(field)) {
                        is_valid = false;
                        validation_error = fmt::format("missing required field: {}", field);
                        break;
                    }
                }
            }
        } catch (const nlohmann::json::exception& e) {
            is_valid = false;
            validation_error = fmt::format("JSON parse failed: {}", e.what());
        }
    }

    // Build result dict
    std::unordered_map<std::string, interpreter::NaabVal> result_dict;
    result_dict["valid"] = interpreter::NaabVal::makeBool(is_valid);
    if (!validation_error.empty()) {
        result_dict["error"] = interpreter::NaabVal::makeString(validation_error);
    }

    // T5: Telemetry for convergence checks
    auto* gov_engine = governance::GovernanceEngine::getCurrent();
    if (gov_engine && gov_engine->getRules().telemetry_output.enabled) {
        std::string pattern_str;
        if (spec.find("pattern") != spec.end() && spec.at("pattern").isString())
            pattern_str = spec.at("pattern").asString();
        gov_engine->writeAgentTelemetry("CONVERGENCE_CHECK", {
            {"valid", is_valid ? "true" : "false"},
            {"pattern", pattern_str},
            {"error", validation_error},
            {"response_length", std::to_string(response.size())},
        });
    }

    return interpreter::NaabVal::makeDict(std::move(result_dict));
}

// ============================================================================
// Orchestra.select_admissible(candidates [, spec])
// ============================================================================
// Pure selection over candidate dicts (from agent.propose() or response dicts
// carrying an "admissibility" section). Filters admissible candidates,
// optionally applies an enforce_convergence-style spec (pattern or
// required_fields) to each candidate's content, and ranks by admissibility
// score (falling back to coherence_score). No API calls, no state mutation.

interpreter::NaabVal orchestraSelectAdmissible(std::vector<interpreter::NaabVal>& args) {
    if (args.empty() || args.size() > 2) {
        throw std::runtime_error(
            "Orchestra error: orchestra.select_admissible() takes 1 or 2 arguments\n\n"
            "  Got: " + std::to_string(args.size()) + " argument(s)\n"
            "  Expected: orchestra.select_admissible(candidates [, spec])\n\n"
            "  Example:\n"
            "    let proposals = agent.propose(h, \"summarize\", 3)\n"
            "    let pick = orchestra.select_admissible(proposals[\"candidates\"])\n");
    }

    // Accept either a candidates list or the whole agent.propose() result dict
    std::vector<interpreter::NaabVal> candidates;
    if (args[0].isList()) {
        candidates = args[0].asListConst();
    } else if (args[0].isDict()) {
        auto& d = args[0].asDictConst();
        auto c_it = d.find("candidates");
        if (c_it != d.end() && c_it->second.isList()) {
            candidates = c_it->second.asListConst();
        }
    }
    if (candidates.empty() && !args[0].isList() &&
        !(args[0].isDict() && args[0].asDictConst().count("candidates"))) {
        throw std::runtime_error(
            "Orchestra error: candidates must be a list or an agent.propose() result\n\n"
            "  Expected: orchestra.select_admissible(candidates [, spec])\n");
    }

    // Optional spec: reuse enforce_convergence validation per candidate
    bool has_spec = args.size() == 2 && args[1].isDict();

    int best_index = -1;
    double best_score = -1.0;
    int admissible_count = 0;

    for (size_t i = 0; i < candidates.size(); i++) {
        if (!candidates[i].isDict()) continue;
        auto& cand = candidates[i].asDictConst();

        // Admissibility flag (default false when section is missing)
        bool admissible = false;
        double score = 0.0;
        auto adm_it = cand.find("admissibility");
        if (adm_it != cand.end() && adm_it->second.isDict()) {
            auto& adm = adm_it->second.asDictConst();
            auto a_it = adm.find("admissible");
            if (a_it != adm.end() && a_it->second.isBool())
                admissible = a_it->second.asBool();
            auto s_it = adm.find("score");
            if (s_it != adm.end() && (s_it->second.isDouble() || s_it->second.isInt()))
                score = s_it->second.isDouble() ? s_it->second.asDouble()
                                                : static_cast<double>(s_it->second.asInt());
            else {
                auto c_it = adm.find("coherence_score");
                if (c_it != adm.end() && c_it->second.isDouble())
                    score = c_it->second.asDouble();
            }
        }
        if (!admissible) continue;

        // Spec check on candidate content
        if (has_spec) {
            std::string content;
            auto content_it = cand.find("content");
            if (content_it != cand.end() && content_it->second.isString())
                content = content_it->second.asString();
            std::vector<interpreter::NaabVal> conv_args = {
                interpreter::NaabVal::makeString(content), args[1]};
            auto conv = orchestraEnforceConvergence(conv_args);
            bool spec_ok = false;
            if (conv.isDict()) {
                auto& cd = conv.asDictConst();
                auto v_it = cd.find("valid");
                if (v_it != cd.end() && v_it->second.isBool())
                    spec_ok = v_it->second.asBool();
            }
            if (!spec_ok) continue;
        }

        admissible_count++;
        if (score > best_score) {
            best_score = score;
            best_index = static_cast<int>(i);
        }
    }

    std::unordered_map<std::string, interpreter::NaabVal> result_dict;
    result_dict["selected"] = best_index >= 0
        ? candidates[best_index] : interpreter::NaabVal::makeNull();
    result_dict["selected_index"] = interpreter::NaabVal::makeInt(best_index);
    result_dict["admissible_count"] = interpreter::NaabVal::makeInt(admissible_count);
    result_dict["total"] = interpreter::NaabVal::makeInt(static_cast<int>(candidates.size()));
    return interpreter::NaabVal::makeDict(std::move(result_dict));
}

// ============================================================================
// Module Interface
// ============================================================================

bool OrchestraModule::hasFunction(const std::string& name) const {
    return name == "sequential_refinement" || name == "consensus_vote" ||
           name == "enforce_convergence" || name == "select_admissible";
}

interpreter::NaabVal OrchestraModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    if (function_name == "sequential_refinement") return orchestraSequentialRefinement(args);
    if (function_name == "consensus_vote") return orchestraConsensusVote(args);
    if (function_name == "enforce_convergence") return orchestraEnforceConvergence(args);
    if (function_name == "select_admissible") return orchestraSelectAdmissible(args);

    throw std::runtime_error(
        fmt::format("Orchestra error: unknown function 'orchestra.{}'\n\n"
            "  Available: sequential_refinement, consensus_vote, enforce_convergence, select_admissible\n",
            function_name));
}

}  // namespace stdlib
}  // namespace naab
