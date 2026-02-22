#pragma once

#include <string>
#include <unordered_map>
#include <memory>

namespace naab {
namespace error {

/**
 * SymbolTableSnapshot: Captures variable state for error context (Phase 4.1.4)
 *
 * Stores current values of variables in scope when an error occurs,
 * allowing error messages to show "x was 5" type information.
 */
struct SymbolTableSnapshot {
    std::unordered_map<std::string, std::string> variables;  // name -> string representation

    void addVariable(const std::string& name, const std::string& value) {
        variables[name] = value;
    }

    bool hasVariable(const std::string& name) const {
        return variables.count(name) > 0;
    }

    std::string getValue(const std::string& name) const {
        auto it = variables.find(name);
        return (it != variables.end()) ? it->second : "<undefined>";
    }
};

} // namespace error
} // namespace naab

