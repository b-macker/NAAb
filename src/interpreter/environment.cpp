// Environment - Variable scoping and storage
// Using Abseil flat_hash_map for efficient lookups

#include "naab/environment.h" // GEMINI FIX: Include the newly created header
#include "naab/interpreter.h" // For Value definition
#include "naab/error_helpers.h" // For error suggestions

namespace naab {
namespace interpreter {

// Implementation of Environment methods (moved from old stub or reimplemented)

void Environment::define(const std::string& name, std::shared_ptr<Value> value) {
    values_[name] = value;
}

std::shared_ptr<Value> Environment::get(const std::string& name) {
    auto it = values_.find(name);
    if (it != values_.end()) {
        return it->second;
    }
    if (parent_) {
        return parent_->get(name);
    }

    // Generate helpful error message with suggestions
    std::string error_msg = "Undefined variable: " + name;
    auto all_names = getAllNames();
    auto suggestion = error::suggestForUndefinedVariable(name, all_names);
    if (!suggestion.empty()) {
        error_msg += "\n  " + suggestion;
    }
    throw std::runtime_error(error_msg);
}

void Environment::set(const std::string& name, std::shared_ptr<Value> value) {
    auto it = values_.find(name);
    if (it != values_.end()) {
        it->second = value;
        return;
    }
    if (parent_) {
        parent_->set(name, value);
        return;
    }

    // Generate helpful error message with suggestions
    std::string error_msg = "Undefined variable: " + name;
    auto all_names = getAllNames();
    auto suggestion = error::suggestForUndefinedVariable(name, all_names);
    if (!suggestion.empty()) {
        error_msg += "\n  " + suggestion;
    }
    throw std::runtime_error(error_msg);
}

bool Environment::has(const std::string& name) const {
    if (values_.find(name) != values_.end()) {
        return true;
    }
    if (parent_) {
        return parent_->has(name);
    }
    return false;
}

std::vector<std::string> Environment::getAllNames() const {
    std::vector<std::string> names;

    // Get names from current scope
    for (const auto& [name, _] : values_) {
        names.push_back(name);
    }

    // Get names from parent scopes
    if (parent_) {
        auto parent_names = parent_->getAllNames();
        names.insert(names.end(), parent_names.begin(), parent_names.end());
    }

    return names;
}

} // namespace interpreter
} // namespace naab