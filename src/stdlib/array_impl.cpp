//
// NAAb Standard Library - Array Module
// Complete implementation with higher-order functions
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include <vector>
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <unordered_set>
#include <memory>

namespace naab {
namespace stdlib {

// Forward declarations
static std::vector<std::shared_ptr<interpreter::Value>> getArray(const std::shared_ptr<interpreter::Value>& val);
static int getInt(const std::shared_ptr<interpreter::Value>& val);
static double getDouble(const std::shared_ptr<interpreter::Value>& val);
static bool getBool(const std::shared_ptr<interpreter::Value>& val);
static std::shared_ptr<interpreter::Value> makeInt(int i);
static std::shared_ptr<interpreter::Value> makeBool(bool b);
static std::shared_ptr<interpreter::Value> makeArray(const std::vector<std::shared_ptr<interpreter::Value>>& arr);
static std::shared_ptr<interpreter::Value> makeNull();
static int compareValues(const std::shared_ptr<interpreter::Value>& a, const std::shared_ptr<interpreter::Value>& b);

bool ArrayModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "length", "push", "pop", "shift", "unshift", "first", "last",
        "map_fn", "filter_fn", "reduce_fn", "find", "slice_arr",
        "reverse", "sort", "contains", "join"
    };
    return functions.count(name) > 0;
}

std::shared_ptr<interpreter::Value> ArrayModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Function 1: length
    if (function_name == "length") {
        if (args.size() != 1) {
            throw std::runtime_error("length() takes exactly 1 argument");
        }
        auto arr = getArray(args[0]);
        return makeInt(static_cast<int>(arr.size()));
    }

    // Function 2: push (mutates array)
    if (function_name == "push") {
        if (args.size() != 2) {
            throw std::runtime_error("push() takes exactly 2 arguments");
        }
        auto arr = getArray(args[0]);
        arr.push_back(args[1]);
        return makeArray(arr);
    }

    // Function 3: pop (returns last element and mutates)
    if (function_name == "pop") {
        if (args.size() != 1) {
            throw std::runtime_error("pop() takes exactly 1 argument");
        }
        auto arr = getArray(args[0]);
        if (arr.empty()) {
            throw std::runtime_error("Cannot pop from empty array");
        }
        auto last = arr.back();
        return last;
    }

    // Function 4: shift (remove from start)
    if (function_name == "shift") {
        if (args.size() != 1) {
            throw std::runtime_error("shift() takes exactly 1 argument");
        }
        auto arr = getArray(args[0]);
        if (arr.empty()) {
            throw std::runtime_error("Cannot shift from empty array");
        }
        auto first = arr.front();
        return first;
    }

    // Function 5: unshift (add to start)
    if (function_name == "unshift") {
        if (args.size() != 2) {
            throw std::runtime_error("unshift() takes exactly 2 arguments");
        }
        auto arr = getArray(args[0]);
        arr.insert(arr.begin(), args[1]);
        return makeArray(arr);
    }

    // Function 6: first (get first element)
    if (function_name == "first") {
        if (args.size() != 1) {
            throw std::runtime_error("first() takes exactly 1 argument");
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
            throw std::runtime_error("last() takes exactly 1 argument");
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
            throw std::runtime_error("join() takes exactly 2 arguments");
        }
        auto arr = getArray(args[0]);
        std::string delimiter = std::visit([](auto&& arg) -> std::string {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return arg;
            } else {
                throw std::runtime_error("Expected string delimiter");
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
            throw std::runtime_error("map_fn() takes exactly 2 arguments (array, function)");
        }
        if (!evaluator_) {
            throw std::runtime_error("map_fn() requires function evaluator to be set");
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
            throw std::runtime_error("filter_fn() takes exactly 2 arguments (array, predicate)");
        }
        if (!evaluator_) {
            throw std::runtime_error("filter_fn() requires function evaluator to be set");
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
            throw std::runtime_error("reduce_fn() takes exactly 3 arguments (array, function, initial)");
        }
        if (!evaluator_) {
            throw std::runtime_error("reduce_fn() requires function evaluator to be set");
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
            throw std::runtime_error("find() takes exactly 2 arguments (array, predicate)");
        }
        if (!evaluator_) {
            throw std::runtime_error("find() requires function evaluator to be set");
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

    // Function 8: slice_arr
    if (function_name == "slice_arr") {
        if (args.size() != 3) {
            throw std::runtime_error("slice_arr() takes exactly 3 arguments");
        }
        auto arr = getArray(args[0]);
        int start = getInt(args[1]);
        int end = getInt(args[2]);

        // Bounds checking
        if (start < 0) start = 0;
        if (end > static_cast<int>(arr.size())) end = arr.size();
        if (start >= end) return makeArray({});

        std::vector<std::shared_ptr<interpreter::Value>> result(
            arr.begin() + start, arr.begin() + end);
        return makeArray(result);
    }

    // Function 9: reverse
    if (function_name == "reverse") {
        if (args.size() != 1) {
            throw std::runtime_error("reverse() takes exactly 1 argument");
        }
        auto arr = getArray(args[0]);
        std::reverse(arr.begin(), arr.end());
        return makeArray(arr);
    }

    // Function 10: sort
    if (function_name == "sort") {
        if (args.size() != 1) {
            throw std::runtime_error("sort() takes exactly 1 argument");
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
            throw std::runtime_error("contains() takes exactly 2 arguments");
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

    throw std::runtime_error("Unknown function: " + function_name);
}

// Helper functions
static std::vector<std::shared_ptr<interpreter::Value>> getArray(const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([](auto&& arg) -> std::vector<std::shared_ptr<interpreter::Value>> {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<interpreter::Value>>>) {
            return arg;
        } else {
            throw std::runtime_error("Expected array value");
        }
    }, val->data);
}

static int getInt(const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([](auto&& arg) -> int {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int>) {
            return arg;
        } else if constexpr (std::is_same_v<T, double>) {
            return static_cast<int>(arg);
        } else {
            throw std::runtime_error("Expected integer value");
        }
    }, val->data);
}

static double getDouble(const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([](auto&& arg) -> double {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, double>) {
            return arg;
        } else if constexpr (std::is_same_v<T, int>) {
            return static_cast<double>(arg);
        } else {
            throw std::runtime_error("Expected numeric value");
        }
    }, val->data);
}

static bool getBool(const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([](auto&& arg) -> bool {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, bool>) {
            return arg;
        } else {
            throw std::runtime_error("Expected boolean value");
        }
    }, val->data);
}

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
