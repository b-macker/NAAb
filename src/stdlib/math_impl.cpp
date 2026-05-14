//
// NAAb Standard Library - Math Module
// Mathematical functions and constants
//

#define _USE_MATH_DEFINES
#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/utils/string_utils.h"
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <memory>
#include <vector>
#include <unordered_set>

namespace naab {
namespace stdlib {

// Forward declaration of helper function
static double getDouble(const interpreter::NaabVal& val);

bool MathModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "PI", "E", "pi", "e",
        "abs", "sqrt", "pow", "floor", "ceil", "round",
        "min", "max", "sin", "cos", "tan"
    };
    return functions.count(name) > 0;
}

interpreter::NaabVal MathModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    // Constants
    if (function_name == "PI") {
        return interpreter::NaabVal::makeDouble(3.14159265358979323846);
    }
    if (function_name == "E" || function_name == "e") {
        return interpreter::NaabVal::makeDouble(2.71828182845904523536);
    }
    if (function_name == "pi") {
        return interpreter::NaabVal::makeDouble(3.14159265358979323846);
    }

    // Function 1: abs
    if (function_name == "abs") {
        if (args.size() != 1) {
            throw std::runtime_error("abs() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);
        return interpreter::NaabVal::makeDouble(std::abs(x));
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
        return interpreter::NaabVal::makeDouble(std::sqrt(x));
    }

    // Function 3: pow
    if (function_name == "pow") {
        if (args.size() != 2) {
            throw std::runtime_error("pow() takes exactly 2 arguments");
        }
        double base = getDouble(args[0]);
        double exp = getDouble(args[1]);
        return interpreter::NaabVal::makeDouble(std::pow(base, exp));
    }

    // Function 4: floor
    if (function_name == "floor") {
        if (args.size() != 1) {
            throw std::runtime_error("floor() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);
        return interpreter::NaabVal::makeInt(static_cast<int>(std::floor(x)));
    }

    // Function 5: ceil
    if (function_name == "ceil") {
        if (args.size() != 1) {
            throw std::runtime_error("ceil() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);
        return interpreter::NaabVal::makeInt(static_cast<int>(std::ceil(x)));
    }

    // Function 6: round
    if (function_name == "round") {
        if (args.size() != 1) {
            throw std::runtime_error("round() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);
        return interpreter::NaabVal::makeInt(static_cast<int>(std::round(x)));
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
        return interpreter::NaabVal::makeDouble(rounded);
    }

    // Function 7: min — returns int when both args are int, float otherwise
    if (function_name == "min") {
        if (args.size() != 2) {
            throw std::runtime_error("min() takes exactly 2 arguments");
        }
        if (args[0].isInt() && args[1].isInt()) {
            return interpreter::NaabVal::makeInt(std::min(args[0].asInt(), args[1].asInt()));
        }
        double a = getDouble(args[0]);
        double b = getDouble(args[1]);
        return interpreter::NaabVal::makeDouble(std::min(a, b));
    }

    // Function 8: max — returns int when both args are int, float otherwise
    if (function_name == "max") {
        if (args.size() != 2) {
            throw std::runtime_error("max() takes exactly 2 arguments");
        }
        if (args[0].isInt() && args[1].isInt()) {
            return interpreter::NaabVal::makeInt(std::max(args[0].asInt(), args[1].asInt()));
        }
        double a = getDouble(args[0]);
        double b = getDouble(args[1]);
        return interpreter::NaabVal::makeDouble(std::max(a, b));
    }

    // Function 9: sin
    if (function_name == "sin") {
        if (args.size() != 1) {
            throw std::runtime_error("sin() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);
        return interpreter::NaabVal::makeDouble(std::sin(x));
    }

    // Function 10: cos
    if (function_name == "cos") {
        if (args.size() != 1) {
            throw std::runtime_error("cos() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);
        return interpreter::NaabVal::makeDouble(std::cos(x));
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

        return interpreter::NaabVal::makeDouble(std::tan(x));
    }

    if (function_name == "random" || function_name == "rand") {
        // math.random() — returns a random float in [0.0, 1.0)
        // math.random(max) — returns a random int in [0, max)
        // math.random(min, max) — returns a random int in [min, max)
        static bool seeded = false;
        if (!seeded) {
            srand(static_cast<unsigned>(time(nullptr)));
            seeded = true;
        }
        if (args.empty()) {
            return interpreter::NaabVal::makeDouble(
                static_cast<double>(rand()) / (static_cast<double>(RAND_MAX) + 1.0));
        } else if (args.size() == 1) {
            int max_val = static_cast<int>(getDouble(args[0]));
            if (max_val <= 0) return interpreter::NaabVal::makeInt(0);
            return interpreter::NaabVal::makeInt(rand() % max_val);
        } else {
            int min_val = static_cast<int>(getDouble(args[0]));
            int max_val = static_cast<int>(getDouble(args[1]));
            if (max_val <= min_val) return interpreter::NaabVal::makeInt(min_val);
            return interpreter::NaabVal::makeInt(min_val + (rand() % (max_val - min_val)));
        }
    }
    if (function_name == "sum" || function_name == "avg" || function_name == "average" || function_name == "mean") {
        throw std::runtime_error(
            "Unknown math function: " + function_name + "\n\n"
            "  NAAb math module doesn't have " + function_name + "().\n"
            "  Use array.reduce_fn() or a polyglot block:\n"
            "    let total = array.reduce_fn(nums, fn(a, b) { return a + b }, 0)\n"
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
static double getDouble(const interpreter::NaabVal& val) {
    if (val.isDouble()) return val.asDouble();
    if (val.isInt()) return static_cast<double>(val.asInt());
    throw std::runtime_error("Expected numeric value");
}

} // namespace stdlib
} // namespace naab
