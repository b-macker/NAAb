//
// TEMPLATE: NAAb Standard Library Module
// Copy this file and customize for new stdlib modules
//
// USAGE:
//   1. Replace {{MODULE_NAME}} with module name (e.g., String, Array, Math)
//   2. Replace {{module_name}} with lowercase name (e.g., string, array, math)
//   3. Add function declarations in private section
//   4. Implement helper functions for type conversion
//   5. Register module in stdlib.cpp
//

#pragma once

#include "naab/stdlib.h"
#include "naab/interpreter.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_set>
#include <stdexcept>

namespace naab {
namespace stdlib {

// ============================================================================
// {{MODULE_NAME}} Module Implementation
// ============================================================================

class {{MODULE_NAME}}Module : public Module {
public:
    // REQUIRED: Return module name (lowercase)
    std::string getName() const override {
        return "{{module_name}}";
    }

    // REQUIRED: Check if function exists
    bool hasFunction(const std::string& name) const override {
        static const std::unordered_set<std::string> functions = {
            // TODO: Add function names here
            // Example: "length", "substring", "concat"
        };
        return functions.count(name) > 0;
    }

    // REQUIRED: Call function by name
    std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override {

        // TODO: Add function dispatching here
        // Example:
        // if (function_name == "length") return length(args);
        // if (function_name == "substring") return substring(args);

        throw std::runtime_error("Unknown function: " + function_name);
    }

private:
    // ========================================================================
    // STDLIB FUNCTION IMPLEMENTATIONS
    // ========================================================================

    // TODO: Declare each stdlib function as private method
    // Pattern:
    // std::shared_ptr<interpreter::Value> function_name(
    //     const std::vector<std::shared_ptr<interpreter::Value>>& args);

    // Example:
    // std::shared_ptr<interpreter::Value> length(
    //     const std::vector<std::shared_ptr<interpreter::Value>>& args) {
    //     if (args.size() != 1) {
    //         throw std::runtime_error("length() takes exactly 1 argument");
    //     }
    //     std::string s = getString(args[0]);
    //     return std::make_shared<interpreter::Value>(static_cast<int>(s.length()));
    // }

    // ========================================================================
    // HELPER FUNCTIONS - Type Conversion
    // ========================================================================

    // Get string from Value using std::visit
    std::string getString(const std::shared_ptr<interpreter::Value>& val) {
        return std::visit([](auto&& arg) -> std::string {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return arg;
            } else {
                throw std::runtime_error("Expected string value");
            }
        }, val->data);
    }

    // Get int from Value using std::visit
    int getInt(const std::shared_ptr<interpreter::Value>& val) {
        return std::visit([](auto&& arg) -> int {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, int>) {
                return arg;
            } else if constexpr (std::is_same_v<T, double>) {
                return static_cast<int>(arg);  // Allow implicit conversion
            } else {
                throw std::runtime_error("Expected integer value");
            }
        }, val->data);
    }

    // Get double from Value using std::visit
    double getDouble(const std::shared_ptr<interpreter::Value>& val) {
        return std::visit([](auto&& arg) -> double {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, double>) {
                return arg;
            } else if constexpr (std::is_same_v<T, int>) {
                return static_cast<double>(arg);  // Allow implicit conversion
            } else {
                throw std::runtime_error("Expected numeric value");
            }
        }, val->data);
    }

    // Get bool from Value using std::visit
    bool getBool(const std::shared_ptr<interpreter::Value>& val) {
        return std::visit([](auto&& arg) -> bool {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, bool>) {
                return arg;
            } else {
                throw std::runtime_error("Expected boolean value");
            }
        }, val->data);
    }

    // Get array from Value using std::visit
    std::vector<std::shared_ptr<interpreter::Value>> getArray(
        const std::shared_ptr<interpreter::Value>& val) {
        return std::visit([](auto&& arg) -> std::vector<std::shared_ptr<interpreter::Value>> {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
                return arg;
            } else {
                throw std::runtime_error("Expected array value");
            }
        }, val->data);
    }

    // Get function from Value using std::visit
    std::shared_ptr<interpreter::FunctionValue> getFunction(
        const std::shared_ptr<interpreter::Value>& val) {
        return std::visit([](auto&& arg) -> std::shared_ptr<interpreter::FunctionValue> {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::shared_ptr<interpreter::FunctionValue>>) {
                return arg;
            } else {
                throw std::runtime_error("Expected function value");
            }
        }, val->data);
    }

    // Get string array (helper for string operations)
    std::vector<std::string> getStringArray(const std::shared_ptr<interpreter::Value>& val) {
        auto arr = getArray(val);
        std::vector<std::string> result;
        result.reserve(arr.size());
        for (const auto& item : arr) {
            result.push_back(getString(item));
        }
        return result;
    }
};

} // namespace stdlib
} // namespace naab
