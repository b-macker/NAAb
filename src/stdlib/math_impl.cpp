//
// NAAb Standard Library - Math Module
// Mathematical functions and constants
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include <cmath>
#include <algorithm>
#include <memory>
#include <unordered_set>

namespace naab {
namespace stdlib {

// Forward declaration of helper function
static double getDouble(const std::shared_ptr<interpreter::Value>& val);

bool MathModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "PI", "E",
        "abs_fn", "sqrt", "pow_fn", "floor", "ceil", "round_fn",
        "min_fn", "max_fn", "sin", "cos", "tan"
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

    // Function 1: abs_fn
    if (function_name == "abs_fn") {
        if (args.size() != 1) {
            throw std::runtime_error("abs_fn() takes exactly 1 argument");
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

    // Function 3: pow_fn
    if (function_name == "pow_fn") {
        if (args.size() != 2) {
            throw std::runtime_error("pow_fn() takes exactly 2 arguments");
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

    // Function 6: round_fn
    if (function_name == "round_fn") {
        if (args.size() != 1) {
            throw std::runtime_error("round_fn() takes exactly 1 argument");
        }
        double x = getDouble(args[0]);
        return std::make_shared<interpreter::Value>(static_cast<int>(std::round(x)));
    }

    // Function 7: min_fn
    if (function_name == "min_fn") {
        if (args.size() != 2) {
            throw std::runtime_error("min_fn() takes exactly 2 arguments");
        }
        double a = getDouble(args[0]);
        double b = getDouble(args[1]);
        return std::make_shared<interpreter::Value>(std::min(a, b));
    }

    // Function 8: max_fn
    if (function_name == "max_fn") {
        if (args.size() != 2) {
            throw std::runtime_error("max_fn() takes exactly 2 arguments");
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

    throw std::runtime_error("Unknown function: " + function_name);
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
