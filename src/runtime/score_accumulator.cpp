#include "naab/score_accumulator.h"
#include "naab/governance.h"
#include "naab/crypto_utils.h"

#include <algorithm>
#include <sstream>
#include <fmt/format.h>

namespace naab {
namespace governance {

ScoreAccumulator::ScoreAccumulator(const ScoringConfig& config)
    : config_(config) {}

int ScoreAccumulator::addFinding(const std::string& rule_name, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Weight lookup — same algorithm as governance_engine.cpp:396-408
    int weight = config_.default_weight;
    auto wit = config_.rule_weights.find(rule_name);
    if (wit != config_.rule_weights.end()) {
        weight = wit->second;
    }
    weight = std::max(0, weight);

    // Saturating add — monotonic, bounded
    if (cumulative_score_ <= SATURATION_LIMIT - weight) {
        cumulative_score_ += weight;
    } else {
        cumulative_score_ = SATURATION_LIMIT;
    }

    contributions_[rule_name] += weight;

    findings_.push_back({rule_name, message, weight, cumulative_score_});

    return weight;
}

int ScoreAccumulator::score() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cumulative_score_;
}

const std::vector<ScoredFinding>& ScoreAccumulator::findings() const {
    // Caller must hold their own synchronization if accessing concurrently
    return findings_;
}

std::string ScoreAccumulator::zone() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cumulative_score_ >= config_.red_threshold) return "red";
    if (cumulative_score_ >= config_.yellow_threshold) return "yellow";
    return "green";
}

std::string ScoreAccumulator::formatBreakdown() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Count occurrences per rule
    std::unordered_map<std::string, int> occurrence_count;
    for (const auto& f : findings_) {
        occurrence_count[f.rule_name]++;
    }

    // Sort by descending contribution — same as governance_engine.cpp:1433-1436
    std::vector<std::pair<std::string, int>> sorted(
        contributions_.begin(), contributions_.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::ostringstream oss;
    for (const auto& [rule, total] : sorted) {
        int count = occurrence_count.count(rule) ? occurrence_count[rule] : 1;
        int per = count > 0 ? total / count : total;
        if (count > 1) {
            oss << fmt::format("    +{:<4} {} ({}x @{})\n", total, rule, count, per);
        } else {
            oss << fmt::format("    +{:<4} {}\n", total, rule);
        }
    }
    return oss.str();
}

bool ScoreAccumulator::verifyIntegrity() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Replay all findings, recompute score — same as governance_engine.cpp:1450-1471
    int recomputed = 0;
    for (const auto& f : findings_) {
        int weight = config_.default_weight;
        auto wit = config_.rule_weights.find(f.rule_name);
        if (wit != config_.rule_weights.end()) {
            weight = wit->second;
        }
        weight = std::max(0, weight);
        if (recomputed <= SATURATION_LIMIT - weight) {
            recomputed += weight;
        } else {
            recomputed = SATURATION_LIMIT;
        }
    }
    return recomputed == cumulative_score_;
}

std::string ScoreAccumulator::integrityHash() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // SHA-256 over the findings sequence for tamper evidence
    std::ostringstream oss;
    for (const auto& f : findings_) {
        oss << f.rule_name << ":" << f.weight << ":" << f.score_after << "\n";
    }
    return security::CryptoUtils::sha256(oss.str());
}

}  // namespace governance
}  // namespace naab
