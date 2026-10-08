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
    // effectiveConfig() rather than getCurrent(): a thread with no sandbox
    // installed must not be a thread with no policy. Absent context denies --
    // this thread's sandbox, else the process policy if one was established,
    // else refuse. That is what makes the gate cover entry points nobody
    // remembered, rather than only the ones that install a ScopedSandbox.
    if (!security::ScopedSandbox::effectiveConfig()
             .hasCapability(security::Capability::BLOCK_CALL)) {
        if (auto* sandbox = security::ScopedSandbox::getCurrent()) {
            sandbox->logViolation("polyglot:" + language, "<block>",
                                  "BLOCK_CALL capability required");
        }
        throw std::runtime_error(language + " execution denied by sandbox");
    }

    // Shell disabled BY THE PROJECT: refuse every runtime that is a separate
    // program. Before this, only the persistent executors (shell, node, ruby)
    // checked for exec, so with shell disabled php, go, cpp and rust still
    // started commands -- by absolute path or execve, which the subprocess PATH
    // restriction does not cover and RLIMIT_NPROC does not bind as root (or at
    // `elevated` at all). Measured with the protection map: those cells were
    // TEXT-ONLY or OPEN with shell disabled. A runtime that is its own process
    // cannot be told "no commands" from here, so the language is refused.
    //
    // Keyed on shell_disabled_by_policy, NOT on !allow_exec: the `standard`
    // level withholds exec from every project in enforce mode, and refusing on
    // that would refuse compute-only blocks in projects that never disabled
    // shell. Only the operator's explicit decision triggers this.
    //
    // Here, not in each executor, for the reason the gate above gives: one
    // place, and a new executor is covered by default (runsInProcess() is false
    // unless the executor says otherwise).
    if (!it->second->runsInProcess() &&
        security::ScopedSandbox::effectiveConfig().shell_disabled_by_policy) {
        if (auto* sandbox = security::ScopedSandbox::getCurrent()) {
            sandbox->logViolation("polyglot:" + language, "<block>",
                                  "shell disabled by project policy");
        }
        throw std::runtime_error(
            "Security: " + language + " execution denied by sandbox\n\n"
            "  This project's governance disables running commands, and " + language + "\n"
            "  code runs as a separate program that can start commands the\n"
            "  sandbox cannot observe, so the language is refused.\n\n"
            "  Help:\n"
            "  - Languages hosted inside the interpreter itself (embedded python,\n"
            "    javascript and sql, where this build includes them) still run,\n"
            "    under the same policy.\n"
            "  - The project owner decides whether commands are allowed, in the\n"
            "    project configuration.\n");
    }

    return it->second.get();
}

std::string LanguageRegistry::runtimeVersion(const std::string& language) const {
    auto it = executors_.find(language);
    if (it == executors_.end()) return "";
    return it->second->getRuntimeVersion();
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
