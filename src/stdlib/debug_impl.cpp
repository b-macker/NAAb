// NAAb Standard Library - Debug Module
// Provides debugging utilities for inspecting complex types,
// scope inspection, timing, snapshot/compare, and more.

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/utils/error_formatter.h"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <unordered_map>
#include <algorithm>

namespace naab {
namespace stdlib {

// Static interpreter pointer for scope inspection (same pattern as bolo_impl.cpp)
static interpreter::Interpreter* g_debug_interpreter = nullptr;

// Static storage for watch values (label -> previous value string)
static std::unordered_map<std::string, std::string> g_watch_values;

// Static storage for timers (label -> start time)
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_timers;

// Static storage for snapshots (label -> {var_name -> value_string})
static std::unordered_map<std::string, std::unordered_map<std::string, std::string>> g_snapshots;

void DebugModule::setInterpreter(interpreter::Interpreter* interp) {
    g_debug_interpreter = interp;
    // Bug 3: Clear stale state from previous runs
    g_watch_values.clear();
    g_timers.clear();
    g_snapshots.clear();
}

// Helper to serialize any value to a debug string
static std::string valueToDebugString(interpreter::NaabVal nval, int indent = 0) {
    std::ostringstream oss;
    std::string indent_str(static_cast<size_t>(indent * 2), ' ');

    if (indent > 20) return "(...)";

    if (nval.isNull()) {
        return "null";
    }

    if (nval.isInt()) {
        oss << nval.asInt();
    } else if (nval.isDouble()) {
        oss << std::fixed << std::setprecision(2) << nval.asDouble();
    } else if (nval.isBool()) {
        oss << (nval.asBool() ? "true" : "false");
    } else if (nval.isString()) {
        oss << "\"" << nval.asString() << "\"";
    } else if (nval.isList()) {
        const auto& arr = nval.asListConst();
        oss << "[";
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << valueToDebugString(arr[i], indent + 1);
        }
        oss << "]";
    } else if (nval.isDict()) {
        const auto& dict = nval.asDictConst();
        std::vector<std::string> sorted_keys;
        for (const auto& [key, value] : dict) sorted_keys.push_back(key);
        std::sort(sorted_keys.begin(), sorted_keys.end());
        oss << "{\n";
        for (size_t i = 0; i < sorted_keys.size(); ++i) {
            if (i > 0) oss << ",\n";
            oss << indent_str << "  \"" << sorted_keys[i] << "\": "
                << valueToDebugString(dict.at(sorted_keys[i]), indent + 1);
        }
        oss << "\n" << indent_str << "}";
    } else if (nval.isStructVal()) {
        const auto& struct_val = nval.asStructConst();
        oss << struct_val->type_name << " {\n";
        for (size_t i = 0; i < struct_val->field_values.size(); ++i) {
            if (i < struct_val->definition->fields.size()) {
                const auto& field = struct_val->definition->fields[i];
                oss << indent_str << "  " << field.name << ": "
                    << valueToDebugString(struct_val->field_values[i], indent + 1);
                if (i < struct_val->field_values.size() - 1) oss << ",";
                oss << "\n";
            }
        }
        oss << indent_str << "}";
    } else if (nval.isFunction()) {
        const auto& func = nval.asFunctionConst();
        oss << "[Function: " << func->name << "(";
        for (size_t i = 0; i < func->params.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << func->params[i];
        }
        oss << ")]";
    } else if (nval.isBlock()) {
        const auto& block = nval.asBlockConst();
        oss << "[Block: " << block->metadata.language << "]";
    } else if (nval.isPythonObject()) {
        const auto& pyobj = nval.asPythonObjectConst();
        oss << "[PythonObject: " << pyobj->repr << "]";
    } else if (nval.isFuture()) {
        const auto& fut = nval.asFutureConst();
        oss << "[Future: " << fut->description << "]";
    } else {
        oss << "[Complex Type]";
    }

    return oss.str();
}

// Helper: get type name string from a value
static std::string getTypeName(interpreter::NaabVal nval) {
    if (nval.isNull()) return "null";
    if (nval.isInt()) return "int";
    if (nval.isDouble()) return "float";
    if (nval.isBool()) return "bool";
    if (nval.isString()) return "string";
    if (nval.isList()) return "array";
    if (nval.isDict()) return "dict";
    if (nval.isStructVal()) return "struct:" + nval.asStructConst()->type_name;
    if (nval.isFunction()) return "function";
    if (nval.isBlock()) return "block";
    if (nval.isPythonObject()) return "python_object";
    if (nval.isFuture()) return "future";
    return "unknown";
}

// Helper: deep diff two values, returns human-readable diff string
static std::string diffValues(interpreter::NaabVal a, interpreter::NaabVal b,
                               const std::string& path = "",
                               int depth = 0) {
    if (depth > 20) return path + "(nested too deeply to compare)";
    std::string prefix = path.empty() ? "" : path + ": ";

    if (a.isNull() && b.isNull()) return "";
    if (a.isNull()) return prefix + "null vs " + valueToDebugString(b);
    if (b.isNull()) return prefix + valueToDebugString(a) + " vs null";

    std::string type_a = getTypeName(a);
    std::string type_b = getTypeName(b);

    if (type_a != type_b) {
        return prefix + "Types differ: " + type_a + " vs " + type_b +
               " (" + valueToDebugString(a) + " vs " + valueToDebugString(b) + ")";
    }

    // Same type — compare by type
    if (a.isInt()) {
        int va = a.asInt(), vb = b.asInt();
        if (va != vb) return prefix + std::to_string(va) + " vs " + std::to_string(vb);
        return "";
    }
    if (a.isDouble()) {
        double va = a.asDouble(), vb = b.asDouble();
        if (va != vb) {
            std::ostringstream oss;
            oss << prefix << std::fixed << std::setprecision(2) << va << " vs " << vb;
            return oss.str();
        }
        return "";
    }
    if (a.isBool()) {
        bool va = a.asBool(), vb = b.asBool();
        if (va != vb) return prefix + std::string(va ? "true" : "false") + " vs " + (vb ? "true" : "false");
        return "";
    }
    if (a.isString()) {
        const auto& va = a.asString();
        const auto& vb = b.asString();
        if (va != vb) return prefix + "\"" + va + "\" vs \"" + vb + "\"";
        return "";
    }

    // Arrays
    if (a.isList()) {
        const auto& arr_a = a.asListConst();
        const auto& arr_b = b.asListConst();
        std::vector<std::string> diffs;
        size_t max_len = std::max(arr_a.size(), arr_b.size());
        if (arr_a.size() != arr_b.size()) {
            diffs.push_back(prefix + "Array lengths differ: " +
                          std::to_string(arr_a.size()) + " vs " + std::to_string(arr_b.size()));
        }
        for (size_t i = 0; i < std::min(arr_a.size(), arr_b.size()); ++i) {
            std::string d = diffValues(arr_a[i], arr_b[i], prefix + "[" + std::to_string(i) + "]", depth + 1);
            if (!d.empty()) diffs.push_back(d);
        }
        for (size_t i = std::min(arr_a.size(), arr_b.size()); i < max_len; ++i) {
            if (i < arr_a.size()) {
                diffs.push_back(prefix + "[" + std::to_string(i) + "] only in first: " + valueToDebugString(arr_a[i]));
            } else {
                diffs.push_back(prefix + "[" + std::to_string(i) + "] only in second: " + valueToDebugString(arr_b[i]));
            }
        }
        std::string result;
        for (size_t i = 0; i < diffs.size(); ++i) {
            if (i > 0) result += "\n";
            result += diffs[i];
        }
        return result;
    }

    // Dicts
    if (a.isDict()) {
        const auto& da = a.asDictConst();
        const auto& db = b.asDictConst();
        std::vector<std::string> diffs;
        for (const auto& [k, v] : da) {
            auto it = db.find(k);
            if (it == db.end()) {
                diffs.push_back("Key '" + k + "' only in first");
            } else {
                std::string d = diffValues(v, it->second, prefix + "." + k, depth + 1);
                if (!d.empty()) diffs.push_back(d);
            }
        }
        for (const auto& [k, v] : db) {
            if (da.find(k) == da.end()) {
                diffs.push_back("Key '" + k + "' only in second");
            }
        }
        std::string result;
        for (size_t i = 0; i < diffs.size(); ++i) {
            if (i > 0) result += "\n";
            result += diffs[i];
        }
        return result;
    }

    // Fallback: compare string representations
    std::string sa = valueToDebugString(a), sb = valueToDebugString(b);
    if (sa != sb) return prefix + sa + " vs " + sb;
    return "";
}


// Debug Module Implementation
bool DebugModule::hasFunction(const std::string& name) const {
    return name == "inspect" || name == "type" || name == "log" ||
           name == "assert" || name == "trace" || name == "keys" ||
           name == "values" || name == "diff" || name == "timer" ||
           name == "env" || name == "stack" || name == "watch" ||
           name == "snapshot" || name == "compare";
}

interpreter::NaabVal DebugModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    // ===== EXISTING FUNCTIONS =====

    if (function_name == "inspect") {
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.inspect", {"value"}, 1, static_cast<int>(args.size()))
            );
        }
        return interpreter::NaabVal::makeString(valueToDebugString(args[0]));

    } else if (function_name == "type") {
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.type", {"value"}, 1, static_cast<int>(args.size()))
            );
        }
        return interpreter::NaabVal::makeString(getTypeName(args[0]));

    // ===== NEW FUNCTIONS =====

    } else if (function_name == "log") {
        // debug.log(label, value) — Print to stderr, return value for chaining
        if (args.size() != 2) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.log", {"label", "value"}, 2, static_cast<int>(args.size()))
            );
        }
        std::string label = args[0].toString();
        fprintf(stderr, "[DEBUG] %s: %s\n", label.c_str(), valueToDebugString(args[1]).c_str());
        fflush(stderr);
        return args[1]; // Return value for chaining

    } else if (function_name == "assert") {
        // debug.assert(condition, message?) — Throw if false
        if (args.empty() || args.size() > 2) {
            throw std::runtime_error(
                "debug.assert() takes 1-2 arguments (condition, message?)\n\n"
                "  Got: " + std::to_string(args.size()) + " arguments\n\n"
                "  Example: debug.assert(x > 0, \"x must be positive\")\n"
            );
        }
        bool cond = args[0].toBool();
        if (!cond) {
            std::string msg = (args.size() > 1) ? args[1].toString() : "Assertion failed";
            throw std::runtime_error("Assertion error: " + msg);
        }
        return interpreter::NaabVal::makeBool(true);

    } else if (function_name == "trace") {
        // debug.trace(value) — Print with file:line to stderr, return value
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.trace", {"value"}, 1, static_cast<int>(args.size()))
            );
        }
        std::string location = "<unknown>";
        if (g_debug_interpreter) {
            // Bug 7: Use call stack info for file:line, not just filename
            auto stack = g_debug_interpreter->getCallStackInfo();
            if (!stack.empty()) {
                location = stack.back();
            } else {
                location = g_debug_interpreter->getCurrentFilename();
                if (location.empty()) location = "<repl>";
            }
        }
        fprintf(stderr, "[TRACE] %s -> %s\n", location.c_str(), valueToDebugString(args[0]).c_str());
        fflush(stderr);
        return args[0]; // Return value for chaining

    } else if (function_name == "keys") {
        // debug.keys(dict|struct) — Return key/field names as array
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.keys", {"value"}, 1, static_cast<int>(args.size()))
            );
        }
        std::vector<interpreter::NaabVal> keys;
        if (args[0].isDict()) {
            for (const auto& [k, v] : args[0].asDictConst()) {
                keys.push_back(interpreter::NaabVal::makeString(k));
            }
        } else if (args[0].isStructVal()) {
            const auto& sv = args[0].asStructConst();
            if (sv->definition) {
                for (const auto& field : sv->definition->fields) {
                    keys.push_back(interpreter::NaabVal::makeString(field.name));
                }
            }
        } else {
            throw std::runtime_error(
                "debug.keys() expects a dict or struct\n\n"
                "  Got: " + getTypeName(args[0]) + "\n"
                "  Expected: dict or struct\n"
            );
        }
        return interpreter::NaabVal::makeList(keys);

    } else if (function_name == "values") {
        // debug.values(dict|struct) — Return values as array
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.values", {"value"}, 1, static_cast<int>(args.size()))
            );
        }
        std::vector<interpreter::NaabVal> vals;
        if (args[0].isDict()) {
            for (const auto& [k, v] : args[0].asDictConst()) {
                vals.push_back(v);
            }
        } else if (args[0].isStructVal()) {
            const auto& sv = args[0].asStructConst();
            vals = sv->field_values;
        } else {
            throw std::runtime_error(
                "debug.values() expects a dict or struct\n\n"
                "  Got: " + getTypeName(args[0]) + "\n"
                "  Expected: dict or struct\n"
            );
        }
        return interpreter::NaabVal::makeList(vals);

    } else if (function_name == "diff") {
        // debug.diff(a, b) — Deep comparison, human-readable diff
        if (args.size() != 2) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.diff", {"a", "b"}, 2, static_cast<int>(args.size()))
            );
        }
        std::string result = diffValues(args[0], args[1]);
        if (result.empty()) result = "(no differences)";
        return interpreter::NaabVal::makeString(result);

    } else if (function_name == "timer") {
        // debug.timer(label) — Toggle start/stop named timer, return ms elapsed
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.timer", {"label"}, 1, static_cast<int>(args.size()))
            );
        }
        std::string label = args[0].toString();
        auto it = g_timers.find(label);
        if (it == g_timers.end()) {
            // Start timer
            g_timers[label] = std::chrono::steady_clock::now();
            fprintf(stderr, "[TIMER] %s: started\n", label.c_str());
            fflush(stderr);
            return interpreter::NaabVal::makeDouble(0.0);
        } else {
            // Stop timer
            auto end = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - it->second).count();
            g_timers.erase(it);
            fprintf(stderr, "[TIMER] %s: %.2fms\n", label.c_str(), ms);
            fflush(stderr);
            return interpreter::NaabVal::makeDouble(ms);
        }

    } else if (function_name == "env") {
        // debug.env() — All variables in current scope as dict
        if (args.size() != 0) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.env", {}, 0, static_cast<int>(args.size()))
            );
        }
        std::unordered_map<std::string, interpreter::NaabVal> result;
        if (g_debug_interpreter) {
            // Bug 1: Filter out internal markers and functions (same as snapshot)
            auto all_vars = g_debug_interpreter->getCurrentScopeVariables();
            for (const auto& [name, val] : all_vars) {
                if (val.isString()) {
                    const auto& s = val.asString();
                    if (s.size() >= 18 && s.substr(0, 18) == "__stdlib_module__:") continue;
                    if (s.size() >= 10 && s.substr(0, 10) == "__module__:") continue;
                }
                if (val.isFunction()) continue;
                result[name] = val;
            }
        }
        return interpreter::NaabVal::makeDict(std::move(result));

    } else if (function_name == "stack") {
        // debug.stack() — Call stack as array of strings
        if (args.size() != 0) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.stack", {}, 0, static_cast<int>(args.size()))
            );
        }
        std::vector<interpreter::NaabVal> frames;
        if (g_debug_interpreter) {
            auto stack_info = g_debug_interpreter->getCallStackInfo();
            for (const auto& frame : stack_info) {
                frames.push_back(interpreter::NaabVal::makeString(frame));
            }
        }
        return interpreter::NaabVal::makeList(std::move(frames));

    } else if (function_name == "watch") {
        // debug.watch(label, value) — Track value changes across calls
        if (args.size() != 2) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.watch", {"label", "value"}, 2, static_cast<int>(args.size()))
            );
        }
        std::string label = args[0].toString();
        std::string current = valueToDebugString(args[1]);
        auto it = g_watch_values.find(label);
        if (it == g_watch_values.end()) {
            // First time seeing this watch
            g_watch_values[label] = current;
            fprintf(stderr, "[WATCH] %s = %s\n", label.c_str(), current.c_str());
        } else if (it->second != current) {
            // Value changed
            fprintf(stderr, "[WATCH] %s CHANGED: %s -> %s\n",
                    label.c_str(), it->second.c_str(), current.c_str());
            it->second = current;
        } else {
            // Unchanged
            fprintf(stderr, "[WATCH] %s = %s (unchanged)\n", label.c_str(), current.c_str());
        }
        fflush(stderr);
        return args[1]; // Return value for chaining

    } else if (function_name == "snapshot") {
        // debug.snapshot(label) — Capture scope state at checkpoint
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.snapshot", {"label"}, 1, static_cast<int>(args.size()))
            );
        }
        std::string label = args[0].toString();
        std::unordered_map<std::string, std::string> snap;
        if (g_debug_interpreter) {
            auto vars = g_debug_interpreter->getCurrentScopeVariables();
            for (const auto& [name, val] : vars) {
                // Skip stdlib module markers
                if (val.isString()) {
                    const auto& s = val.asString();
                    if (s.size() >= 18 && s.substr(0, 18) == "__stdlib_module__:") continue;
                    if (s.size() >= 10 && s.substr(0, 10) == "__module__:") continue;
                }
                // Skip functions
                if (val.isFunction()) continue;
                snap[name] = valueToDebugString(val);
            }
        }
        g_snapshots[label] = snap;
        fprintf(stderr, "[SNAPSHOT] '%s' captured (%zu variables)\n", label.c_str(), snap.size());
        fflush(stderr);
        return interpreter::NaabVal::makeBool(true);

    } else if (function_name == "compare") {
        // debug.compare(label1, label2) — Diff two snapshots
        if (args.size() != 2) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.compare", {"label1", "label2"}, 2, static_cast<int>(args.size()))
            );
        }
        std::string l1 = args[0].toString(), l2 = args[1].toString();
        auto it1 = g_snapshots.find(l1);
        auto it2 = g_snapshots.find(l2);
        if (it1 == g_snapshots.end()) {
            throw std::runtime_error("Snapshot '" + l1 + "' not found. Call debug.snapshot(\"" + l1 + "\") first.");
        }
        if (it2 == g_snapshots.end()) {
            throw std::runtime_error("Snapshot '" + l2 + "' not found. Call debug.snapshot(\"" + l2 + "\") first.");
        }

        const auto& s1 = it1->second;
        const auto& s2 = it2->second;
        std::ostringstream oss;

        // Check for added/removed/changed variables
        for (const auto& [name, val] : s2) {
            auto prev = s1.find(name);
            if (prev == s1.end()) {
                oss << "  Added: " << name << " = " << val << "\n";
            } else if (prev->second != val) {
                oss << "  Changed: " << name << ": " << prev->second << " -> " << val << "\n";
            } else {
                oss << "  " << name << " unchanged: " << val << "\n";
            }
        }
        for (const auto& [name, val] : s1) {
            if (s2.find(name) == s2.end()) {
                oss << "  Removed: " << name << " = " << val << "\n";
            }
        }

        std::string result = oss.str();
        if (result.empty()) result = "(no changes between snapshots)";
        return interpreter::NaabVal::makeString(result);
    }

    // ===== HELPER ERRORS FOR COMMON MISTAKES =====

    if (function_name == "print" || function_name == "println" || function_name == "dump" || function_name == "pp") {
        throw std::runtime_error(
            "Unknown debug function: " + function_name + "\n\n"
            "  Did you mean: debug.log(label, value)?\n"
            "  debug.log prints to stderr and returns the value for chaining.\n\n"
            "  Example: let x = debug.log(\"result\", some_func())\n"
        );
    }
    if (function_name == "typeof" || function_name == "class" || function_name == "kind") {
        throw std::runtime_error(
            "Unknown debug function: " + function_name + "\n\n"
            "  Did you mean: debug.type(value)?\n"
            "  Example: print(debug.type(x))  // -> \"int\", \"string\", \"array\", etc.\n"
        );
    }
    if (function_name == "vars" || function_name == "locals" || function_name == "scope" || function_name == "variables") {
        throw std::runtime_error(
            "Unknown debug function: " + function_name + "\n\n"
            "  Did you mean: debug.env()?\n"
            "  Returns a dict of all variables in the current scope.\n"
        );
    }
    if (function_name == "time" || function_name == "stopwatch" || function_name == "benchmark") {
        throw std::runtime_error(
            "Unknown debug function: " + function_name + "\n\n"
            "  Did you mean: debug.timer(label)?\n"
            "  Call once to start, call again with same label to stop and print elapsed time.\n"
        );
    }
    if (function_name == "assertEqual" || function_name == "assert_eq" || function_name == "expect") {
        throw std::runtime_error(
            "Unknown debug function: " + function_name + "\n\n"
            "  Did you mean: debug.assert(condition, message)?\n"
            "  Example: debug.assert(x == 42, \"x should be 42\")\n"
        );
    }

    // Generic fallback with full function list
    throw std::runtime_error(
        "Unknown debug function: " + function_name + "\n\n"
        "  Available debug functions:\n"
        "    debug.log(label, val)       Log value to stderr, return val\n"
        "    debug.inspect(val)          Pretty-print any value\n"
        "    debug.type(val)             Get type name\n"
        "    debug.assert(cond, msg?)    Assert condition\n"
        "    debug.trace(val)            Log with file:line, return val\n"
        "    debug.keys(val)             Dict/struct key names\n"
        "    debug.values(val)           Dict/struct values\n"
        "    debug.diff(a, b)            Compare two values\n"
        "    debug.timer(label)          Start/stop named timer\n"
        "    debug.env()                 All variables in scope\n"
        "    debug.stack()               Call stack\n"
        "    debug.watch(label, val)     Track value changes\n"
        "    debug.snapshot(label)       Capture scope state\n"
        "    debug.compare(l1, l2)       Diff two snapshots\n"
    );
}

} // namespace stdlib
} // namespace naab
