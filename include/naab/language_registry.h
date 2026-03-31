#pragma once

// NAAb Language Registry
// Centralized registry for language-specific block executors

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "naab/naab_val.h"

namespace naab {
namespace runtime {

// Abstract base class for language executors
class Executor {
public:
    virtual ~Executor() = default;

    // Execute code and store in runtime context
    virtual bool execute(const std::string& code) = 0;

    // Execute code and return the result value
    virtual interpreter::NaabVal executeWithReturn(
        const std::string& code) = 0;

    // Call a function in the executor
    virtual interpreter::NaabVal callFunction(
        const std::string& function_name,
        const std::vector<interpreter::NaabVal>& args
    ) = 0;

    // Check if executor is initialized
    virtual bool isInitialized() const = 0;

    // Get language name
    virtual std::string getLanguage() const = 0;

    // Get captured stdout/stderr from the executor
    virtual std::string getCapturedOutput() = 0;

    // Get runtime version string (e.g., "Python 3.11.2")
    // Default returns empty string. Override in concrete executors.
    virtual std::string getRuntimeVersion() const { return ""; }
};

// Language Registry - manages language-specific executors
class LanguageRegistry {
public:
    LanguageRegistry();
    ~LanguageRegistry() = default;

    // Register a language executor
    void registerExecutor(const std::string& language,
                          std::unique_ptr<Executor> executor);

    // Get executor for a language (returns nullptr if not found)
    Executor* getExecutor(const std::string& language);

    // Check if a language is supported
    bool isSupported(const std::string& language) const;

    // Get list of supported languages
    std::vector<std::string> supportedLanguages() const;

    // Remove a language executor
    void unregisterExecutor(const std::string& language);

    // Get singleton instance
    static LanguageRegistry& instance();

private:
    std::unordered_map<std::string, std::unique_ptr<Executor>> executors_;

    // Singleton instance
    static LanguageRegistry* instance_;
};

} // namespace runtime
} // namespace naab

