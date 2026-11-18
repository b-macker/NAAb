#pragma once
// The time one polyglot block may run: languages.per_language.<lang>.timeout,
// else limits.timeout.per_block (GovernanceEngine::blockTimeLimitSeconds()) --
// and the time all of a run's polyglot blocks may run together:
// limits.timeout.total_polyglot (GovernanceEngine::polyglotTimeRemainingMs()).
//
// The per-block keys were read by a getter nothing called, so a block ran as
// long as it liked under either (A2); total_polyglot was parsed and read by
// nothing at all (A175). underBlockTimeLimit() runs a block under both on
// every route that executes one: under enforce a nested, thread-local
// ScopedTimeout interrupts it at the tighter of the two (it only ever tightens
// the run's own deadline; audit mode reports and interrupts nothing). A block
// that ran past its own limit -- interrupted or not, returned or thrown -- is
// refused through enforce() at HARD; so is a block that spent the last of the
// run's polyglot time, and a block that starts with none left does not run.
// Every block's time is charged to the run under an active engine, whether a
// total is configured or not, so a total a reload adds judges the whole run.
// A route that executes foreign code must call it, or these keys stop nothing
// there.
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

#include "naab/governance.h"
#include "naab/resource_limits.h"

namespace naab {
namespace governance {

template <typename Fn>
void underBlockTimeLimit(GovernanceEngine* gov, const std::string& language, int line, Fn&& fn) {
    if (!gov || !gov->isActive()) {
        fn();
        return;
    }
    const int limit = gov->blockTimeLimitSeconds(language);   // 0: no limit of its own
    const long long total_left = gov->polyglotTimeRemainingMs();  // -1: no total configured
    if (total_left == 0) {
        // The run's polyglot time is spent: the block does not run.
        std::string err = gov->checkPolyglotTimeTotal(language, line);
        if (!err.empty()) throw std::runtime_error(err);
    }
    long long arm_ms = limit > 0 ? limit * 1000LL : 0;
    if (total_left > 0 && (arm_ms == 0 || total_left < arm_ms)) arm_ms = total_left;

    const auto start = std::chrono::steady_clock::now();
    auto elapsed_ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
    };
    auto ran_past = [&] {
        return limit > 0 && std::chrono::steady_clock::now() - start >= std::chrono::seconds(limit);
    };
    // Charges the block's time; true when that spent the last of the total.
    auto charge = [&] { return gov->chargePolyglotTime(elapsed_ms()); };
    {
        // Audit mode reports and stops nothing, so it does not interrupt.
        std::optional<security::ScopedTimeout> timer;
        if (arm_ms > 0 && gov->getMode() == GovernanceMode::ENFORCE) {
            timer.emplace(std::chrono::milliseconds(arm_ms),
                          security::ScopedTimeout::Scope::Local);
        }
        try {
            fn();
        } catch (const GovernanceHardError&) {
            timer.reset();
            charge();
            throw;
        } catch (...) {
            timer.reset();
            const bool spent = charge();
            if (ran_past()) gov->checkBlockTimeLimit(language, limit, line);  // HARD: throws
            if (spent) gov->checkPolyglotTimeTotal(language, line);           // HARD: throws
            throw;
        }
    }
    const bool spent = charge();
    if (ran_past()) {
        std::string err = gov->checkBlockTimeLimit(language, limit, line);
        if (!err.empty()) throw std::runtime_error(err);
    }
    if (spent) {
        std::string err = gov->checkPolyglotTimeTotal(language, line);
        if (!err.empty()) throw std::runtime_error(err);
    }
}

}  // namespace governance
}  // namespace naab
