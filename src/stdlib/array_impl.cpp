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
#include "naab/limits.h"
#include <sstream>

namespace naab {
namespace stdlib {

// Forward declarations
static void ensureArray(const interpreter::NaabVal& val, const std::string& func_name);
static std::vector<interpreter::NaabVal> getArray(const interpreter::NaabVal& val);
static int getInt(const interpreter::NaabVal& val);
// Note: getDouble and getBool removed - unused
static interpreter::NaabVal makeInt(int i);
static interpreter::NaabVal makeBool(bool b);
static interpreter::NaabVal makeArray(const std::vector<interpreter::NaabVal>& arr);
static interpreter::NaabVal makeNull();
static int compareValues(const interpreter::NaabVal& a, const interpreter::NaabVal& b);

bool ArrayModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "length", "push", "pop", "shift", "unshift", "first", "last",
        "map_fn", "filter_fn", "reduce_fn", "find", "index_of", "slice_arr", "slice",
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

interpreter::NaabVal ArrayModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

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

    // Function 2: push (mutates array in-place)
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
        ensureArray(args[0], "array.push");
        // V-GOV-023: enforce array size limits on native push
        limits::checkArraySize(args[0].asList().size() + 1);
        args[0].asList().push_back(args[1]);
        return args[0];
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
        ensureArray(args[0], "array.pop");
        auto& arr = args[0].asList();  // Mutate in-place via shared_ptr
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
        arr.pop_back();  // Remove the last element in-place

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
        ensureArray(args[0], "array.shift");
        auto& arr = args[0].asList();
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
        arr.erase(arr.begin());  // Remove the first element in-place
        return first;
    }

    // Function 5: unshift (add to start, mutates in-place)
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
        ensureArray(args[0], "array.unshift");
        // V-GOV-023: enforce array size limits on native unshift
        limits::checkArraySize(args[0].asList().size() + 1);
        args[0].asList().insert(args[0].asList().begin(), args[1]);
        return args[0];
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
        if (!args[1].isString()) {
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
        std::string delimiter = args[1].asString();

        if (arr.empty()) {
            return interpreter::NaabVal::makeString("");
        }

        std::string result = arr[0].toString();
        for (size_t i = 1; i < arr.size(); ++i) {
            result += delimiter + arr[i].toString();
        }
        return interpreter::NaabVal::makeString(result);
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

        std::vector<interpreter::NaabVal> result;
        result.reserve(arr.size());

        for (const auto& elem : arr) {
            // Call the function with each element
            auto mapped_value = evaluator_(fn, {elem});
            result.push_back(mapped_value);
        }

        return makeArray(result);
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

        std::vector<interpreter::NaabVal> result;

        for (const auto& elem : arr) {
            // Call the predicate with each element
            auto filter_result = evaluator_(predicate, {elem});
            // Include element if predicate returns true
            if (filter_result.toBool()) {
                result.push_back(elem);
            }
        }

        return makeArray(result);
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
            if (find_result.toBool()) {
                return elem;
            }
        }

        // Return null/void if not found
        return makeNull();
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

        std::vector<interpreter::NaabVal> result(
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
        if (args.size() < 1 || args.size() > 2) {
            throw std::runtime_error(
                "array.sort() takes 1 or 2 arguments (array, comparator?)\n\n"
                "  Example:\n"
                "    arr.sort()                           // default sort\n"
                "    arr.sort(fn(a, b) { return a - b })  // custom comparator\n"
            );
        }
        auto& arr = args[0].asList();

        if (args.size() == 2 && evaluator_) {
            // Sort with comparator function
            // V-UB-001: Wrap in try/catch to prevent UB from exceptions mid-sort,
            // and validate return type to maintain strict weak ordering.
            auto comp_fn = args[1];
            bool sort_error = false;
            std::sort(arr.begin(), arr.end(),
                [this, &comp_fn, &sort_error](const interpreter::NaabVal& a, const interpreter::NaabVal& b) {
                    if (sort_error) return false;  // Consistent ordering after error
                    try {
                        auto res = evaluator_(comp_fn, {a, b});
                        if (res.isInt()) return res.asInt() < 0;
                        if (res.isDouble()) return res.asDouble() < 0;
                        sort_error = true;
                        return false;
                    } catch (...) {
                        sort_error = true;
                        return false;
                    }
                });
            if (sort_error) {
                throw std::runtime_error(
                    "array.sort() comparator must return a number (negative, zero, or positive)");
            }
        } else {
            // Default sort using value comparison
            std::sort(arr.begin(), arr.end(),
                [](const auto& a, const auto& b) {
                    return compareValues(a, b) < 0;
                });
        }
        return args[0];
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

    // Aliases: map/filter/reduce -> map_fn/filter_fn/reduce_fn
    if (function_name == "map") {
        return call("map_fn", args);
    }
    if (function_name == "filter") {
        return call("filter_fn", args);
    }
    if (function_name == "reduce") {
        return call("reduce_fn", args);
    }

    if (function_name == "append" || function_name == "add") {
        throw std::runtime_error(
            "Unknown array function: " + function_name + "\n\n"
            "  Did you mean: array.push()?\n"
            "  Example: array.push(my_array, new_item)\n"
            "  Note: push() mutates the original array.\n"
        );
    }
    if (function_name == "includes") {
        throw std::runtime_error(
            "Unknown array function: includes\n\n"
            "  Did you mean: array.contains()?\n"
            "  Example: array.contains(my_array, value)  // true/false\n"
        );
    }
    if (function_name == "remove" || function_name == "delete") {
        throw std::runtime_error(
            "Unknown array function: " + function_name + "\n\n"
            "  NAAb arrays don't have remove(). Use filter_fn to create a filtered copy:\n"
            "    let filtered = array.filter_fn(arr, fn(x) { return x != unwanted })\n"
        );
    }
    if (function_name == "len" || function_name == "size" || function_name == "count") {
        throw std::runtime_error(
            "Unknown array function: " + function_name + "\n\n"
            "  Did you mean: array.length()? Or use the built-in len():\n"
            "    array.length(my_array)  // module function\n"
            "    len(my_array)           // built-in (simpler)\n"
        );
    }
    if (function_name == "index_of") {
        if (args.size() != 2) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "array.index_of",
                    {"array", "value"},
                    2,
                    static_cast<int>(args.size())
                )
            );
        }
        auto arr = getArray(args[0]);
        auto target = args[1];
        for (int i = 0; i < static_cast<int>(arr.size()); ++i) {
            if (compareValues(arr[i], target) == 0) {
                return makeInt(i);
            }
        }
        return makeInt(-1);
    }
    if (function_name == "indexOf" || function_name == "findIndex") {
        throw std::runtime_error(
            "Unknown array function: " + function_name + "\n\n"
            "  Did you mean: array.index_of(arr, value)?\n"
        );
    }

    if (function_name == "insert") {
        throw std::runtime_error(
            "Unknown array function: insert\n\n"
            "  Did you mean: array.push()?\n"
            "  NAAb arrays use push() to add elements.\n"
            "  Example: array.push(my_array, new_item)\n"
        );
    }
    if (function_name == "concat" || function_name == "extend" || function_name == "merge") {
        throw std::runtime_error(
            "Unknown array function: " + function_name + "\n\n"
            "  NAAb doesn't have array concat. Use the + operator:\n"
            "    let combined = arr1 + arr2\n"
        );
    }
    if (function_name == "flat" || function_name == "flatten") {
        throw std::runtime_error(
            "Unknown array function: " + function_name + "\n\n"
            "  NAAb doesn't have flatten. Use a polyglot block:\n"
            "    let flat = <<python\n[x for sub in nested for x in sub]\n    >>\n"
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

// Validation-only helper for in-place mutators (push/pop/shift/unshift)
static void ensureArray(const interpreter::NaabVal& val, const std::string& func_name) {
    if (!val.isList()) {
        throw std::runtime_error(
            "Type error: Expected array, got " + val.getTypeName() + "\n\n"
            "  Help:\n"
            "  - " + func_name + " requires an array argument\n"
            "  - Create an array with: [1, 2, 3]\n"
            "  - Check the type with: typeof(value)\n\n"
            "  Example:\n"
            "    ✗ Wrong: " + func_name + "(\"hello\")  // string is not an array\n"
            "    ✓ Right: " + func_name + "([1, 2, 3])  // array\n"
        );
    }
}

// Helper functions
static std::vector<interpreter::NaabVal> getArray(const interpreter::NaabVal& val) {
    if (val.isList()) {
        // Return a copy of the list
        return std::vector<interpreter::NaabVal>(val.asListConst().begin(), val.asListConst().end());
    }
    throw std::runtime_error(
        "Type error: Expected array, got " + val.getTypeName() + "\n\n"
        "  Help:\n"
        "  - Array module functions require array arguments\n"
        "  - Create an array with: [1, 2, 3]\n"
        "  - Check the type with: typeof(value)\n\n"
        "  Example:\n"
        "    ✗ Wrong: array.length(\"hello\")  // string\n"
        "    ✓ Right: array.length([1, 2, 3])  // array\n"
    );
}

static int getInt(const interpreter::NaabVal& val) {
    if (val.isInt()) return val.asInt();
    if (val.isDouble()) return static_cast<int>(val.asDouble());
    throw std::runtime_error(
        "Type error: Expected integer, got " + val.getTypeName() + "\n\n"
        "  Help:\n"
        "  - Array indices must be integers\n"
        "  - Numeric parameters require int or float\n"
        "  - Convert with: int(value)\n\n"
        "  Example:\n"
        "    ✗ Wrong: array.slice_arr(arr, \"0\", \"5\")  // string\n"
        "    ✓ Right: array.slice_arr(arr, 0, 5)  // int\n"
    );
}

static interpreter::NaabVal makeInt(int i) {
    return interpreter::NaabVal::makeInt(i);
}

static interpreter::NaabVal makeBool(bool b) {
    return interpreter::NaabVal::makeBool(b);
}

static interpreter::NaabVal makeArray(const std::vector<interpreter::NaabVal>& arr) {
    return interpreter::NaabVal::makeList(arr);
}

static interpreter::NaabVal makeNull() {
    return interpreter::NaabVal::makeNull();
}

// Compare two values (for sorting and equality)
static int compareValues(const interpreter::NaabVal& a, const interpreter::NaabVal& b) {
    // Null comparison
    if (a.isNull() && b.isNull()) return 0;
    if (a.isNull()) return -1;
    if (b.isNull()) return 1;

    // Numeric comparison (int/double interop)
    bool a_num = a.isInt() || a.isDouble();
    bool b_num = b.isInt() || b.isDouble();
    if (a_num && b_num) {
        double a_val = a.isInt() ? static_cast<double>(a.asInt()) : a.asDouble();
        double b_val = b.isInt() ? static_cast<double>(b.asInt()) : b.asDouble();
        if (a_val < b_val) return -1;
        if (a_val > b_val) return 1;
        return 0;
    }

    // Bool comparison
    if (a.isBool() && b.isBool()) {
        if (a.asBool() == b.asBool()) return 0;
        return a.asBool() ? 1 : -1;
    }

    // String comparison
    if (a.isString() && b.isString()) {
        return a.asString().compare(b.asString());
    }

    // Different types - compare by type name
    return a.getTypeName().compare(b.getTypeName());
}

} // namespace stdlib
} // namespace naab
