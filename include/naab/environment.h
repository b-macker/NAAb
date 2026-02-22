#pragma once

#include "absl/container/flat_hash_map.h"
#include <string>
#include <memory>
#include <vector> // GEMINI FIX: Required for getAllNames

namespace naab {
namespace interpreter {

// Forward declaration of Value
class Value;

class Environment {
public:
    // GEMINI FIX: Added constructor for parent environment
    Environment(std::shared_ptr<Environment> parent = nullptr) : parent_(parent) {}

    // Defines a new variable in the current scope
    void define(const std::string& name, std::shared_ptr<Value> value);

    // Retrieves a variable from the current or parent scopes
    std::shared_ptr<Value> get(const std::string& name);

    // Sets an existing variable in the current or parent scopes
    void set(const std::string& name, std::shared_ptr<Value> value);

    // Checks if a variable exists in the current or parent scopes
    bool has(const std::string& name) const;

    // Retrieves all names defined in this and parent scopes (for error suggestions)
    std::vector<std::string> getAllNames() const;

    // Returns the parent environment
    std::shared_ptr<Environment> getParent() const { return parent_; }

private:
    // Using Abseil flat_hash_map for efficient lookups
    absl::flat_hash_map<std::string, std::shared_ptr<Value>> values_; // GEMINI FIX: Store shared_ptr<Value>
    std::shared_ptr<Environment> parent_;
};

} // namespace interpreter
} // namespace naab

