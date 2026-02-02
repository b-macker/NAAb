#include "naab/environment.h"
#include <stdexcept>

namespace naab {
namespace interpreter {

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

    throw std::runtime_error("Undefined variable: " + name);
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

    throw std::runtime_error("Undefined variable: " + name);
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

    // Collect names from current scope
    for (const auto& [name, value] : values_) {
        names.push_back(name);
    }

    // Collect names from parent scopes
    if (parent_) {
        auto parent_names = parent_->getAllNames();
        names.insert(names.end(), parent_names.begin(), parent_names.end());
    }

    return names;
}

} // namespace interpreter
} // namespace naab
