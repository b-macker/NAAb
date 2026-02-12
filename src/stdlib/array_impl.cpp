//
// NAAb Standard Library - Array Module
// Complete implementation with higher-order functions
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/utils/string_utils.h"
#include "naab/utils/error_formatter.h"
#include <vector>
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <unordered_set>
#include <memory>
#include <sstream>

namespace naab {
namespace stdlib {

// Forward declarations
static std::vector<std::shared_ptr<interpreter::Value>> getArray(const std::shared_ptr<interpreter::Value>& val);
static int getInt(const std::shared_ptr<interpreter::Value>& val);
// Note: getDouble and getBool removed - unused
static std::shared_ptr<interpreter::Value> makeInt(int i);
static std::shared_ptr<interpreter::Value> makeBool(bool b);
static std::shared_ptr<interpreter::Value> makeArray(const std::vector<std::shared_ptr<interpreter::Value>>& arr);
static std::shared_ptr<interpreter::Value> makeNull();
static int compareValues(const std::shared_ptr<interpreter::Value>& a, const std::shared_ptr<interpreter::Value>& b);

bool ArrayModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "length", "push", "pop", "shift", "unshift", "first", "last",
        "map_fn", "filter_fn", "reduce_fn", "find", "slice_arr", "slice",
        "reverse", "sort", "contains", "join"
    };
    return functions.count(name) > 0;
}

bool ArrayModule::isMutatingFunction(const std::string& name) const {
    static const std::unordered_set<std::string> mutating_funcs = {
        "push", "pop", "shift", "unshift", "reverse", "sort"
    };
    return mutating_funcs.count(name) > 0;
}

std::shared_ptr<interpreter::Value> ArrayModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Function 1: length
    if (function_name == "length") {
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.length",
                    {"array"},
                    1,
                    static_cast<int>(args.size())
                )
            );
        }
        auto arr = getArray(args[0]);
        return makeInt(static_cast<int>(arr.size()));
    }

    // Function 2: push (mutates array)
    if (function_name == "push") {
        if (args.size() != 2) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.push",
                    {"array", "element"},
                    2,
                    static_cast<int>(args.size())
                )
            );
        }
        auto arr = getArray(args[0]);
        arr.push_back(args[1]);
        return makeArray(arr);
    }

    // Function 3: pop (returns last element and mutates)
    if (function_name == "pop") {
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.pop",
                    {"array"},
                    1,
                    static_cast<int>(args.size())
                )
            );
        }
        auto arr = getArray(args[0]);
        if (arr.empty()) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatEmptyCollectionError(
                    "array.pop",
                    "array",
                    "array.length"
                )
            );
        }
        auto last = arr.back();
        arr.pop_back();  // Remove the last element

        // Store the modified array back in args[0] so auto-mutation can use it
        // Create a special marker by modifying the first argument
        args[0]->data = arr;

        return last;  // Return the popped element
    }

    // Function 4: shift (remove from start)
    if (function_name == "shift") {
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.shift",
                    {"array"},
                    1,
                    static_cast<int>(args.size())
                )
            );
        }
        auto arr = getArray(args[0]);
        if (arr.empty()) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatEmptyCollectionError(
                    "array.shift",
                    "array",
                    "array.length"
                )
            );
        }
        auto first = arr.front();
        arr.erase(arr.begin());  // Remove the first element

        // Store the modified array back in args[0] so auto-mutation can use it
        args[0]->data = arr;

        return first;  // Return the shifted element
    }

    // Function 5: unshift (add to start)
    if (function_name == "unshift") {
        if (args.size() != 2) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.unshift",
                    {"array", "element"},
                    2,
                    static_cast<int>(args.size())
                )
            );
        }
        auto arr = getArray(args[0]);
        arr.insert(arr.begin(), args[1]);
        return makeArray(arr);
    }

    // Function 6: first (get first element)
    if (function_name == "first") {
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.first",
                    {"array"},
                    1,
                    static_cast<int>(args.size())
                )
            );
        }
        auto arr = getArray(args[0]);
        if (arr.empty()) {
            return makeNull();
        }
        return arr.front();
    }

    // Function 7: last (get last element)
    if (function_name == "last") {
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.last",
                    {"array"},
                    1,
                    static_cast<int>(args.size())
                )
            );
        }
        auto arr = getArray(args[0]);
        if (arr.empty()) {
            return makeNull();
        }
        return arr.back();
    }

    // Function 8: join (join array to string)
    if (function_name == "join") {
        if (args.size() != 2) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.join",
                    {"array", "delimiter"},
                    2,
                    static_cast<int>(args.size())
                )
            );
        }
        auto arr = getArray(args[0]);
        std::string delimiter = std::visit([](auto&& arg) -> std::string {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return arg;
            } else {
                throw std::runtime_error(
                    "Type error: array.join delimiter must be a string\n\n"
                    "  Help:\n"
                    "  - Second argument should be a string delimiter\n"
                    "  - Common delimiters: \", \", \" \", \"-\", etc.\n\n"
                    "  Example:\n"
                    "    ✗ Wrong: array.join([1, 2, 3], 123)\n"
                    "    ✓ Right: array.join([1, 2, 3], \", \")\n"
                );
            }
        }, args[1]->data);

        if (arr.empty()) {
            return std::make_shared<interpreter::Value>(std::string(""));
        }

        std::string result = arr[0]->toString();
        for (size_t i = 1; i < arr.size(); ++i) {
            result += delimiter + arr[i]->toString();
        }
        return std::make_shared<interpreter::Value>(result);
    }

    // Function 9: map_fn (higher-order function)
    if (function_name == "map_fn") {
        if (args.size() != 2) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.map_fn",
                    {"array", "function"},
                    2,
                    static_cast<int>(args.size())
                )
            );
        }
        if (!evaluator_) {
            throw std::runtime_error(
                "Internal error: array.map_fn requires function evaluator\n\n"
                "  This is likely a bug in the NAAb interpreter.\n"
                "  Please report this issue.\n"
            );
        }

        auto arr = getArray(args[0]);
        auto fn = args[1];  // Function value

        std::vector<std::shared_ptr<interpreter::Value>> result;
        result.reserve(arr.size());

        for (const auto& elem : arr) {
            // Call the function with each element
            auto mapped_value = evaluator_(fn, {elem});
            result.push_back(mapped_value);
        }

        return std::make_shared<interpreter::Value>(result);
    }

    // Function 5: filter_fn (higher-order function)
    if (function_name == "filter_fn") {
        if (args.size() != 2) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.filter_fn",
                    {"array", "predicate"},
                    2,
                    static_cast<int>(args.size())
                )
            );
        }
        if (!evaluator_) {
            throw std::runtime_error(
                "Internal error: array.filter_fn requires function evaluator\n\n"
                "  This is likely a bug in the NAAb interpreter.\n"
                "  Please report this issue.\n"
            );
        }

        auto arr = getArray(args[0]);
        auto predicate = args[1];  // Function value

        std::vector<std::shared_ptr<interpreter::Value>> result;

        for (const auto& elem : arr) {
            // Call the predicate with each element
            auto filter_result = evaluator_(predicate, {elem});
            // Include element if predicate returns true
            if (filter_result->toBool()) {
                result.push_back(elem);
            }
        }

        return std::make_shared<interpreter::Value>(result);
    }

    // Function 6: reduce_fn (higher-order function)
    if (function_name == "reduce_fn") {
        if (args.size() != 3) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.reduce_fn",
                    {"array", "function", "initial"},
                    3,
                    static_cast<int>(args.size())
                )
            );
        }
        if (!evaluator_) {
            throw std::runtime_error(
                "Internal error: array.reduce_fn requires function evaluator\n\n"
                "  This is likely a bug in the NAAb interpreter.\n"
                "  Please report this issue.\n"
            );
        }

        auto arr = getArray(args[0]);
        auto reducer = args[1];  // Function value
        auto accumulator = args[2];  // Initial value

        for (const auto& elem : arr) {
            // Call the reducer with accumulator and current element
            accumulator = evaluator_(reducer, {accumulator, elem});
        }

        return accumulator;
    }

    // Function 7: find (higher-order function)
    if (function_name == "find") {
        if (args.size() != 2) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.find",
                    {"array", "predicate"},
                    2,
                    static_cast<int>(args.size())
                )
            );
        }
        if (!evaluator_) {
            throw std::runtime_error(
                "Internal error: array.find requires function evaluator\n\n"
                "  This is likely a bug in the NAAb interpreter.\n"
                "  Please report this issue.\n"
            );
        }

        auto arr = getArray(args[0]);
        auto predicate = args[1];  // Function value

        for (const auto& elem : arr) {
            // Call the predicate with each element
            auto find_result = evaluator_(predicate, {elem});
            // Return first element where predicate returns true
            if (find_result->toBool()) {
                return elem;
            }
        }

        // Return null/void if not found
        return std::make_shared<interpreter::Value>();
    }

    // Function 8: slice_arr (also accessible as "slice" for convenience)
    if (function_name == "slice_arr" || function_name == "slice") {
        if (args.size() != 3) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.slice_arr",
                    {"array", "start", "end"},
                    3,
                    static_cast<int>(args.size())
                )
            );
        }
        auto arr = getArray(args[0]);
        int start = getInt(args[1]);
        int end = getInt(args[2]);

        // Bounds checking
        if (start < 0) start = 0;
        if (end > static_cast<int>(arr.size())) end = static_cast<int>(arr.size());
        if (start >= end) return makeArray({});

        std::vector<std::shared_ptr<interpreter::Value>> result(
            arr.begin() + start, arr.begin() + end);
        return makeArray(result);
    }

    // Function 9: reverse
    if (function_name == "reverse") {
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.reverse",
                    {"array"},
                    1,
                    static_cast<int>(args.size())
                )
            );
        }
        auto arr = getArray(args[0]);
        std::reverse(arr.begin(), arr.end());
        return makeArray(arr);
    }

    // Function 10: sort
    if (function_name == "sort") {
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.sort",
                    {"array"},
                    1,
                    static_cast<int>(args.size())
                )
            );
        }
        auto arr = getArray(args[0]);

        // Sort using value comparison
        std::sort(arr.begin(), arr.end(),
            [](const auto& a, const auto& b) {
                return compareValues(a, b) < 0;
            });
        return makeArray(arr);
    }

    // Function 11: contains
    if (function_name == "contains") {
        if (args.size() != 2) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.contains",
                    {"array", "element"},
                    2,
                    static_cast<int>(args.size())
                )
            );
        }
        auto arr = getArray(args[0]);
        auto target = args[1];

        for (const auto& item : arr) {
            if (compareValues(item, target) == 0) {
                return makeBool(true);
            }
        }
        return makeBool(false);
    }

    // Common LLM mistakes: map/filter/reduce without _fn suffix
    if (function_name == "map" || function_name == "filter" || function_name == "reduce") {
        std::string correct = function_name + "_fn";
        throw std::runtime_error(
            "Unknown array function: " + function_name + "\n\n"
            "  Help: NAAb uses '" + correct + "' instead of '" + function_name + "'.\n"
            "  Higher-order array functions require the _fn suffix.\n\n"
            "  Example:\n"
            "    fn double(x: int) -> int { return x * 2 }\n"
            "    let doubled = array." + correct + "([1, 2, 3], double)\n"
        );
    }

    if (function_name == "forEach" || function_name == "for_each" || function_name == "each") {
        throw std::runtime_error(
            "Unknown array function: " + function_name + "\n\n"
            "  Help: NAAb uses 'for...in' loops instead of forEach:\n\n"
            "    for item in my_array {\n"
            "        print(item)\n"
            "    }\n"
        );
    }

    // Unknown function - provide helpful error with suggestions
    static const std::vector<std::string> FUNCTIONS = {
        "length", "push", "pop", "shift", "unshift", "first", "last",
        "map_fn", "filter_fn", "reduce_fn", "find", "slice_arr", "slice",
        "reverse", "sort", "contains", "join"
    };

    auto similar = naab::utils::findSimilar(function_name, FUNCTIONS);
    std::string suggestion = naab::utils::formatSuggestions(function_name, similar);

    std::ostringstream oss;
    oss << "Unknown array function: " << function_name << suggestion
        << "\n\n  Available: ";
    for (size_t i = 0; i < FUNCTIONS.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << FUNCTIONS[i];
    }

    throw std::runtime_error(oss.str());
}

// Helper functions
static std::vector<std::shared_ptr<interpreter::Value>> getArray(const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([&val](auto&& arg) -> std::vector<std::shared_ptr<interpreter::Value>> {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
            return arg;
        } else {
            // Get type name for error message
            std::string actual_type = std::visit([](auto&& v) -> std::string {
                using VT = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<VT, std::monostate>) return "null";
                else if constexpr (std::is_same_v<VT, int>) return "int";
                else if constexpr (std::is_same_v<VT, double>) return "float";
                else if constexpr (std::is_same_v<VT, bool>) return "bool";
                else if constexpr (std::is_same_v<VT, std::string>) return "string";
                else return "unknown";
            }, val->data);

            throw std::runtime_error(
                "Type error: Expected array, got " + actual_type + "\n\n"
                "  Help:\n"
                "  - Array module functions require array arguments\n"
                "  - Create an array with: [1, 2, 3]\n"
                "  - Check the type with: typeof(value)\n\n"
                "  Example:\n"
                "    ✗ Wrong: array.length(\"hello\")  // string\n"
                "    ✓ Right: array.length([1, 2, 3])  // array\n"
            );
        }
    }, val->data);
}

static int getInt(const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([&val](auto&& arg) -> int {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int>) {
            return arg;
        } else if constexpr (std::is_same_v<T, double>) {
            return static_cast<int>(arg);
        } else {
            // Get type name for error message
            std::string actual_type = std::visit([](auto&& v) -> std::string {
                using VT = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<VT, std::monostate>) return "null";
                else if constexpr (std::is_same_v<VT, bool>) return "bool";
                else if constexpr (std::is_same_v<VT, std::string>) return "string";
                else if constexpr (std::is_same_v<VT, std::vector<std::shared_ptr<interpreter::Value>>>) return "array";
                else return "unknown";
            }, val->data);

            throw std::runtime_error(
                "Type error: Expected integer, got " + actual_type + "\n\n"
                "  Help:\n"
                "  - Array indices must be integers\n"
                "  - Numeric parameters require int or float\n"
                "  - Convert with: int(value)\n\n"
                "  Example:\n"
                "    ✗ Wrong: array.slice_arr(arr, \"0\", \"5\")  // string\n"
                "    ✓ Right: array.slice_arr(arr, 0, 5)  // int\n"
            );
        }
    }, val->data);
}

// Note: getDouble() and getBool() helper functions removed (unused)

static std::shared_ptr<interpreter::Value> makeInt(int i) {
    return std::make_shared<interpreter::Value>(i);
}

static std::shared_ptr<interpreter::Value> makeBool(bool b) {
    return std::make_shared<interpreter::Value>(b);
}

static std::shared_ptr<interpreter::Value> makeArray(const std::vector<std::shared_ptr<interpreter::Value>>& arr) {
    return std::make_shared<interpreter::Value>(arr);
}

static std::shared_ptr<interpreter::Value> makeNull() {
    return std::make_shared<interpreter::Value>();
}

// Compare two values (for sorting and equality)
static int compareValues(const std::shared_ptr<interpreter::Value>& a, const std::shared_ptr<interpreter::Value>& b) {
    // Get type indices for comparison
    size_t a_index = a->data.index();
    size_t b_index = b->data.index();

    // If different types, compare by type index
    if (a_index != b_index) {
        // Special case: int (1) and double (2) can be compared
        if ((a_index == 1 && b_index == 2) || (a_index == 2 && b_index == 1)) {
            double a_val = (a_index == 1) ? std::get<int>(a->data) : std::get<double>(a->data);
            double b_val = (b_index == 1) ? std::get<int>(b->data) : std::get<double>(b->data);
            if (a_val < b_val) return -1;
            if (a_val > b_val) return 1;
            return 0;
        }
        return (a_index < b_index) ? -1 : 1;
    }

    // Same types - compare values
    return std::visit([](auto&& a_val, auto&& b_val) -> int {
        using A = std::decay_t<decltype(a_val)>;
        using B = std::decay_t<decltype(b_val)>;

        if constexpr (std::is_same_v<A, std::monostate> && std::is_same_v<B, std::monostate>) {
            return 0;
        }
        else if constexpr (std::is_same_v<A, int> && std::is_same_v<B, int>) {
            if (a_val < b_val) return -1;
            if (a_val > b_val) return 1;
            return 0;
        }
        else if constexpr (std::is_same_v<A, double> && std::is_same_v<B, double>) {
            if (a_val < b_val) return -1;
            if (a_val > b_val) return 1;
            return 0;
        }
        else if constexpr (std::is_same_v<A, bool> && std::is_same_v<B, bool>) {
            if (a_val == b_val) return 0;
            return a_val ? 1 : -1;
        }
        else if constexpr (std::is_same_v<A, std::string> && std::is_same_v<B, std::string>) {
            return a_val.compare(b_val);
        }
        else {
            // Complex types: vectors, maps, etc. - treat as equal
            return 0;
        }
    }, a->data, b->data);
}

} // namespace stdlib
} // namespace naab
