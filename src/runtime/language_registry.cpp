// NAAb Language Registry Implementation
// Manages language-specific block executors

#include "naab/language_registry.h"
#include <fmt/core.h>
#include <stdexcept>
#include <algorithm>

namespace naab {
namespace runtime {

// Initialize singleton instance
LanguageRegistry* LanguageRegistry::instance_ = nullptr;

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
    if (!instance_) {
        instance_ = new LanguageRegistry();
    }
    return *instance_;
}

} // namespace runtime
} // namespace naab
