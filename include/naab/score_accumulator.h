#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

namespace naab {
namespace governance {

struct ScoringConfig;  // forward decl from governance.h

struct ScoredFinding {
    std::string rule_name;
    std::string message;
    std::string source;      // which agent/source submitted this finding
    int weight;              // resolved weight (after config lookup + clamp)
    int score_after;         // cumulative score after this finding
};

class ScoreAccumulator {
public:
    static constexpr int SATURATION_LIMIT = 100000;

    explicit ScoreAccumulator(const ScoringConfig& config);

    // Add a finding — returns the weight that was applied
    int addFinding(const std::string& rule_name, const std::string& message,
                   const std::string& source = "");

    // Source tracking
    int sourceCount() const;

    // Read state (all const, thread-safe with internal mutex)
    int score() const;
    const std::vector<ScoredFinding>& findings() const;
    std::string zone() const;          // "green", "yellow", or "red"
    std::string formatBreakdown() const;
    bool verifyIntegrity() const;      // recompute from findings, compare to score
    std::string integrityHash() const; // SHA-256 of findings sequence

private:
    const ScoringConfig& config_;
    mutable std::mutex mutex_;
    int cumulative_score_ = 0;
    std::vector<ScoredFinding> findings_;
    std::unordered_map<std::string, int> contributions_;
    std::unordered_set<std::string> sources_;  // unique non-empty sources
};

}  // namespace governance
}  // namespace naab
