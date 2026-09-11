// NAAb Language Registry Implementation
// Manages language-specific block executors

#include "naab/language_registry.h"
#include "naab/sandbox.h"
#include <fmt/core.h>
#include <stdexcept>
#include <algorithm>

namespace naab {
namespace runtime {

// Singleton instance now uses function-local static (C++11 thread-safe)

LanguageRegistry::LanguageRegistry() {
    // Language registry initialized (silent)
}

void LanguageRegistry::registerExecutor(const std::string& language,
                                         std::unique_ptr<Executor> executor) {
    if (!executor) {
        throw std::invalid_argument("Cannot register null executor");
    }

    if (executors_.find(language) != executors_.end()) {
        fmt::print("[WARN] Overwriting existing executor for language: {}\n", language);
    }

    executors_[language] = std::move(executor);
}

Executor* LanguageRegistry::getExecutor(const std::string& language) {
    auto it = executors_.find(language);
    if (it == executors_.end()) {
        fmt::print("[ERROR] No executor found for language: {}\n", language);
        return nullptr;
    }

    // ONE gate for every language, here rather than in each executor.
    //
    // The capability check used to be written per executor, sixteen times, and
    // most of the copies were wrong: several never had it, and the one in
    // GenericSubprocessExecutor sat on execute() while polyglot blocks go
    // through executeWithReturn(). Measured on 96acf93 under a "restricted"
    // sandbox, cpp, php and rust all ran and wrote files. The defect was never
    // a badly written check; it was that adding a language did not require
    // writing one at all.
    //
    // Every execution path reaches an executor through this function, and
    // availability questions have isSupported()/supportedLanguages(), so this
    // is the narrowest place that covers all of them. A language registered
    // tomorrow is gated the moment it is registered.
    //
    // The "sandbox &&" half preserves today's meaning of no active sandbox.
    // That default is its own problem and its own change; this one is about
    // languages, not entry points.
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(security::Capability::BLOCK_CALL)) {
        sandbox->logViolation("polyglot:" + language, "<block>",
                              "BLOCK_CALL capability required");
        throw std::runtime_error(language + " execution denied by sandbox");
    }

    return it->second.get();
}

bool LanguageRegistry::isSupported(const std::string& language) const {
    return executors_.find(language) != executors_.end();
}

std::vector<std::string> LanguageRegistry::supportedLanguages() const {
    std::vector<std::string> languages;
    languages.reserve(executors_.size());

    for (const auto& pair : executors_) {
        languages.push_back(pair.first);
    }

    // Sort for consistent output
    std::sort(languages.begin(), languages.end());

    return languages;
}

void LanguageRegistry::unregisterExecutor(const std::string& language) {
    auto it = executors_.find(language);
    if (it != executors_.end()) {
        executors_.erase(it);
    }
}

LanguageRegistry& LanguageRegistry::instance() {
    static LanguageRegistry instance;
    return instance;
}

} // namespace runtime
} // namespace naab
