// Provenance for generated code (Bundle 14).
//
// When a program executes code -- a polyglot block, codegen.run() -- the
// telemetry event records the SHA-256 of the text executed, and whether that
// exact text is something a model wrote in this process: an agent reply, the
// reply as agent.send() returned it, a proposal candidate, a string argument of
// a tool call, or what agent.extract_code() took out of one of those.
//
// The rule is EXACT equality of text, by hash. It records that the executed
// text EQUALS a model output, never that it was derived from one: code a
// program edits, trims or joins before running it reads "unattributed", and a
// short text a model and a program both wrote reads as a match. An audit record
// must not hold a guess, so nothing looser is attempted.
//
// Hashes only: no model text is kept here or written to telemetry.
//
// Process-wide and bounded. One program runs per process (REST and MCP run each
// request in a worker process); a tree-walker async interpreter builds its own
// engine, so an engine member would split one run's outputs. The oldest entry
// is dropped at kMaxEntries, which can only turn a match into "unattributed".
#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "naab/crypto_utils.h"

namespace naab::governance {

struct ModelOutputOrigin {
    std::string model;            // the model that answered
    std::string request_sha256;   // the request body as sent (may be empty)
    std::string content_sha256;   // the model's whole reply, before any stripping
    std::string config_name;      // the agent's name in govern.json
    int handle_id = -1;
    int turn_at_request = -1;     // the AGENT_RESPONSE / CDD_TURN join key
    std::string derivation;       // response | returned_content | proposal |
                                  // tool_argument | extract_code
};

class CodeProvenanceLedger {
public:
    static constexpr size_t kMaxEntries = 4096;

    static CodeProvenanceLedger& instance() {
        static CodeProvenanceLedger ledger;
        return ledger;
    }

    // Record `text` as written by a model. Empty text is not recorded.
    void record(const std::string& text, const ModelOutputOrigin& origin) {
        if (text.empty()) return;
        recordHash(security::CryptoUtils::sha256(text), origin);
    }

    // `output` was taken out of `input` by the engine (agent.extract_code).
    // Recorded only when `input` is itself a recorded model output.
    void recordDerived(const std::string& input, const std::string& output,
                       const std::string& derivation) {
        if (input.empty() || output.empty()) return;
        ModelOutputOrigin origin;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = entries_.find(security::CryptoUtils::sha256(input));
            if (it == entries_.end()) return;
            origin = it->second.origin;
        }
        origin.derivation = derivation;
        record(output, origin);
    }

    // True when `text_sha256` is a recorded model output. `distinct_replies` is
    // the number of different model replies the same text was recorded from.
    bool lookup(const std::string& text_sha256, ModelOutputOrigin& out,
                size_t& distinct_replies) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(text_sha256);
        if (it == entries_.end()) return false;
        out = it->second.origin;
        distinct_replies = it->second.replies.size();
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    // Tests only.
    void clearForTesting() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        order_.clear();
    }

private:
    struct Entry {
        ModelOutputOrigin origin;                 // the first reply it came from
        std::unordered_set<std::string> replies;  // content_sha256 of each
    };

    void recordHash(const std::string& hash, const ModelOutputOrigin& origin) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(hash);
        if (it != entries_.end()) {
            // The same text from another reply: keep the first origin, count
            // the replies (bounded, so a repeating model cannot grow it).
            if (it->second.replies.size() < 16)
                it->second.replies.insert(origin.content_sha256);
            return;
        }
        if (entries_.size() >= kMaxEntries && !order_.empty()) {
            entries_.erase(order_.front());
            order_.pop_front();
        }
        Entry e;
        e.origin = origin;
        e.replies.insert(origin.content_sha256);
        entries_.emplace(hash, std::move(e));
        order_.push_back(hash);
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::deque<std::string> order_;
};

// The fields an execution event carries for the code it ran. `code` is the
// text executed. Always sets code_sha256 and code_attribution; the origin_*
// fields only on a match.
inline void addCodeProvenanceFields(
        std::unordered_map<std::string, std::string>& fields,
        const std::string& code) {
    const std::string hash = security::CryptoUtils::sha256(code);
    fields["code_sha256"] = hash;
    ModelOutputOrigin o;
    size_t replies = 0;
    if (code.empty() || !CodeProvenanceLedger::instance().lookup(hash, o, replies)) {
        fields["code_attribution"] = "unattributed";
        return;
    }
    fields["code_attribution"] = "model_output_match";
    fields["origin_derivation"] = o.derivation;
    fields["origin_model"] = o.model;
    fields["origin_request_sha256"] = o.request_sha256;
    fields["origin_content_sha256"] = o.content_sha256;
    fields["origin_config_name"] = o.config_name;
    fields["origin_handle_id"] = std::to_string(o.handle_id);
    fields["origin_turn_at_request"] = std::to_string(o.turn_at_request);
    fields["origin_distinct_replies"] = std::to_string(replies);
}

}  // namespace naab::governance
