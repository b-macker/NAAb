#pragma once
// The time one polyglot block may run: languages.per_language.<lang>.timeout,
// else limits.timeout.per_block (GovernanceEngine::blockTimeLimitSeconds()).
//
// Both keys were read by a getter nothing called, so a block ran as long as it
// liked under either (A2). underBlockTimeLimit() runs a block under that limit
// on every route that executes one: under enforce a nested, thread-local
// ScopedTimeout interrupts it (it only ever tightens the run's own deadline;
// audit mode reports and interrupts nothing), and a block
// that ran past the limit -- interrupted or not, returned or thrown -- is
// refused through enforce() at HARD. A route that executes foreign code must
// call it, or the two keys stop nothing there.
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
    const int limit = (gov && gov->isActive()) ? gov->blockTimeLimitSeconds(language) : 0;
    if (limit <= 0) {
        fn();
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    auto ran_past = [&] {
        return std::chrono::steady_clock::now() - start >= std::chrono::seconds(limit);
    };
    {
        // Audit mode reports and stops nothing, so it does not interrupt.
        std::optional<security::ScopedTimeout> timer;
        if (gov->getMode() == GovernanceMode::ENFORCE) {
            timer.emplace(std::chrono::milliseconds(limit * 1000LL),
                          security::ScopedTimeout::Scope::Local);
        }
        try {
            fn();
        } catch (const GovernanceHardError&) {
            throw;
        } catch (...) {
            timer.reset();
            if (ran_past()) gov->checkBlockTimeLimit(language, limit, line);  // HARD: throws
            throw;
        }
    }
    if (ran_past()) {
        std::string err = gov->checkBlockTimeLimit(language, limit, line);
        if (!err.empty()) throw std::runtime_error(err);
    }
}

}  // namespace governance
}  // namespace naab
