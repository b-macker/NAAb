//
// NAAb Standard Library - Math Module
// Mathematical functions and constants
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/utils/string_utils.h"
#include <cmath>
#include <algorithm>
#include <memory>
#include <vector>
#include <unordered_set>

namespace naab {
namespace stdlib {

// Forward declaration of helper function
static double getDouble(const std::shared_ptr<interpreter::Value>& val);

bool MathModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "PI", "E",
        "abs", "sqrt", "pow", "floor", "ceil", "round",
        "min", "max", "sin", "cos", "tan"
    };
    return functions.count(name) > 0;
}

std::shared_ptr<interpreter::Value> MathModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Constants
    if (function_name == "PI") {
        return std::make_shared<interpreter::Value>(3.14159265358979323846);
    }
    if (function_name == "E") {
        return std::make_shared<interpreter::Value>(2.71828182845904523536);
    }

    // Function 1: abs
    if (function_name == "abs") {
        if (args.size() != 1) {
            throw std::runtime_error("abs() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);
        return std::make_shared<interpreter::Value>(std::abs(x));
    }

    // Function 2: sqrt
    if (function_name == "sqrt") {
        if (args.size() != 1) {
            throw std::runtime_error("sqrt() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);
        if (x < 0) {
            throw std::runtime_error("sqrt() requires non-negative argument");
        }
        return std::make_shared<interpreter::Value>(std::sqrt(x));
    }

    // Function 3: pow
    if (function_name == "pow") {
        if (args.size() != 2) {
            throw std::runtime_error("pow() takes exactly 2 arguments");
        }
        double base = getDouble(args[0]);
        double exp = getDouble(args[1]);
        return std::make_shared<interpreter::Value>(std::pow(base, exp));
    }

    // Function 4: floor
    if (function_name == "floor") {
        if (args.size() != 1) {
            throw std::runtime_error("floor() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);
        return std::make_shared<interpreter::Value>(static_cast<int>(std::floor(x)));
    }

    // Function 5: ceil
    if (function_name == "ceil") {
        if (args.size() != 1) {
            throw std::runtime_error("ceil() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);
        return std::make_shared<interpreter::Value>(static_cast<int>(std::ceil(x)));
    }

    // Function 6: round
    if (function_name == "round") {
        if (args.size() != 1) {
            throw std::runtime_error("round() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);
        return std::make_shared<interpreter::Value>(static_cast<int>(std::round(x)));
    }

    // Function 6b: round_to - round to N decimal places
    if (function_name == "round_to") {
        if (args.size() != 2) {
            throw std::runtime_error(
                "Argument error: math.round_to() takes exactly 2 arguments\n\n"
                "  Expected: math.round_to(value, decimal_places)\n\n"
                "  Example:\n"
                "    math.round_to(3.14159, 2)  // returns 3.14\n"
            );
        }
        double x = getDouble(args[0]);
        int places = static_cast<int>(getDouble(args[1]));
        double factor = std::pow(10.0, places);
        double rounded = std::round(x * factor) / factor;
        return std::make_shared<interpreter::Value>(rounded);
    }

    // Function 7: min
    if (function_name == "min") {
        if (args.size() != 2) {
            throw std::runtime_error("min() takes exactly 2 arguments");
        }
        double a = getDouble(args[0]);
        double b = getDouble(args[1]);
        return std::make_shared<interpreter::Value>(std::min(a, b));
    }

    // Function 8: max
    if (function_name == "max") {
        if (args.size() != 2) {
            throw std::runtime_error("max() takes exactly 2 arguments");
        }
        double a = getDouble(args[0]);
        double b = getDouble(args[1]);
        return std::make_shared<interpreter::Value>(std::max(a, b));
    }

    // Function 9: sin
    if (function_name == "sin") {
        if (args.size() != 1) {
            throw std::runtime_error("sin() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);
        return std::make_shared<interpreter::Value>(std::sin(x));
    }

    // Function 10: cos
    if (function_name == "cos") {
        if (args.size() != 1) {
            throw std::runtime_error("cos() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);
        return std::make_shared<interpreter::Value>(std::cos(x));
    }

    // Function 11: tan
    if (function_name == "tan") {
        if (args.size() != 1) {
            throw std::runtime_error("tan() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);

        // Check for asymptotes at π/2 + nπ (where tan is undefined)
        double mod_pi = std::fmod(std::abs(x), M_PI);
        if (std::abs(mod_pi - M_PI/2.0) < 1e-10) {
            throw std::runtime_error("tan() undefined at π/2 + nπ (asymptote)");
        }

        return std::make_shared<interpreter::Value>(std::tan(x));
    }

    // Common LLM mistakes
    if (function_name == "random" || function_name == "rand") {
        throw std::runtime_error(
            "Unknown math function: " + function_name + "\n\n"
            "  NAAb math module doesn't have random().\n"
            "  Use the crypto module for random numbers:\n"
            "    crypto.random_int(1, 100)    // random int in range\n"
            "    crypto.random_string(16)     // random string\n"
        );
    }
    if (function_name == "log" || function_name == "ln" || function_name == "log2" || function_name == "log10") {
        throw std::runtime_error(
            "Unknown math function: " + function_name + "\n\n"
            "  Logarithm functions are not yet implemented in NAAb.\n"
            "  Use a polyglot block:\n"
            "    let result = <<python\nimport math\nmath." + function_name + "(value)\n    >>\n"
        );
    }

    // Known functions for suggestions
    static const std::vector<std::string> FUNCTIONS = {
        "PI", "E", "abs", "sqrt", "pow", "floor", "ceil", "round", "round_to",
        "min", "max", "sin", "cos", "tan"
    };

    // Special case: common constant casing mistakes
    if (function_name == "pi" || function_name == "Pi") {
        throw std::runtime_error(
            "Unknown math function: " + function_name + "\n\n"
            "  Did you mean: math.PI (uppercase, no parentheses)?\n\n"
            "  Constants are accessed without ():\n"
            "    ✗ Wrong: math.pi()  or  math.PI()\n"
            "    ✓ Right: math.PI\n"
        );
    }
    if (function_name == "e") {
        throw std::runtime_error(
            "Unknown math function: " + function_name + "\n\n"
            "  Did you mean: math.E (uppercase, no parentheses)?\n\n"
            "  Constants are accessed without ():\n"
            "    ✗ Wrong: math.e()  or  math.E()\n"
            "    ✓ Right: math.E\n"
        );
    }

    auto similar = naab::utils::findSimilar(function_name, FUNCTIONS);
    std::string suggestion = naab::utils::formatSuggestions(function_name, similar);

    throw std::runtime_error(
        "Unknown math function: " + function_name + "\n" + suggestion
    );
}

// Helper function
static double getDouble(const std::shared_ptr<interpreter::Value>& val) {
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

} // namespace stdlib
} // namespace naab
