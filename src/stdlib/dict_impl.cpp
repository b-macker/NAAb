//
// NAAb Standard Library - Dict Module
// keys(), values(), has_key() for dictionary values
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace naab {
namespace stdlib {

bool DictModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "keys", "values", "has_key", "get", "get_or", "merge", "entries", "size"
    };
    return functions.count(name) > 0;
}

interpreter::NaabVal DictModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    if (function_name == "keys") {
        if (args.size() != 1 || !args[0].isDict()) {
            throw std::runtime_error(
                "dict.keys requires a single dict argument\n\n"
                "  Example: let k = dict.keys(my_dict)\n"
            );
        }
        const auto& d = args[0].asDictConst();
        std::vector<interpreter::NaabVal> keys;
        keys.reserve(d.size());
        for (const auto& [k, v] : d) {
            keys.push_back(interpreter::NaabVal::makeString(k));
        }
        return interpreter::NaabVal::makeList(std::move(keys));
    }

    if (function_name == "values") {
        if (args.size() != 1 || !args[0].isDict()) {
            throw std::runtime_error(
                "dict.values requires a single dict argument\n\n"
                "  Example: let v = dict.values(my_dict)\n"
            );
        }
        const auto& d = args[0].asDictConst();
        std::vector<interpreter::NaabVal> vals;
        vals.reserve(d.size());
        for (const auto& [k, v] : d) {
            vals.push_back(v);
        }
        return interpreter::NaabVal::makeList(std::move(vals));
    }

    if (function_name == "has_key") {
        if (args.size() != 2 || !args[0].isDict() || !args[1].isString()) {
            throw std::runtime_error(
                "dict.has_key requires (dict, string) arguments\n\n"
                "  Example: let found = dict.has_key(my_dict, \"key\")\n"
            );
        }
        const auto& d = args[0].asDictConst();
        return interpreter::NaabVal::makeBool(d.count(args[1].asString()) > 0);
    }

    if (function_name == "get") {
        if (args.size() != 2 || !args[0].isDict()) {
            throw std::runtime_error(
                "dict.get requires (dict, key) arguments\n\n"
                "  Example: let v = dict.get(my_dict, \"key\")  // returns null if missing\n"
            );
        }
        const auto& d = args[0].asDictConst();
        std::string key = args[1].toString();
        auto it = d.find(key);
        if (it != d.end()) return it->second;
        return interpreter::NaabVal::makeNull();
    }

    if (function_name == "get_or") {
        if (args.size() != 3 || !args[0].isDict()) {
            throw std::runtime_error(
                "dict.get_or requires (dict, key, default) arguments\n\n"
                "  Example: let v = dict.get_or(my_dict, \"key\", 0)\n"
            );
        }
        const auto& d = args[0].asDictConst();
        std::string key = args[1].toString();
        auto it = d.find(key);
        if (it != d.end()) return it->second;
        return args[2];
    }

    if (function_name == "merge") {
        if (args.size() != 2 || !args[0].isDict() || !args[1].isDict()) {
            throw std::runtime_error(
                "dict.merge requires (dict, dict) arguments\n\n"
                "  Example: let combined = dict.merge(d1, d2)\n"
            );
        }
        auto copy = args[0].asDict();
        for (const auto& [k, v] : args[1].asDictConst()) {
            copy[k] = v;
        }
        return interpreter::NaabVal::makeDict(std::move(copy));
    }

    if (function_name == "entries") {
        if (args.size() != 1 || !args[0].isDict()) {
            throw std::runtime_error(
                "dict.entries requires a single dict argument\n\n"
                "  Example: let pairs = dict.entries(my_dict)  // [[k1,v1], [k2,v2]]\n"
            );
        }
        const auto& d = args[0].asDictConst();
        std::vector<interpreter::NaabVal> entries;
        entries.reserve(d.size());
        for (const auto& [k, v] : d) {
            std::vector<interpreter::NaabVal> pair;
            pair.push_back(interpreter::NaabVal::makeString(k));
            pair.push_back(v);
            entries.push_back(interpreter::NaabVal::makeList(std::move(pair)));
        }
        return interpreter::NaabVal::makeList(std::move(entries));
    }

    if (function_name == "size") {
        if (args.size() != 1 || !args[0].isDict()) {
            throw std::runtime_error(
                "dict.size requires a single dict argument\n\n"
                "  Example: let n = dict.size(my_dict)\n"
            );
        }
        return interpreter::NaabVal::makeInt(static_cast<int>(args[0].asDictConst().size()));
    }

    throw std::runtime_error("Unknown dict function: " + function_name + "\n\n"
        "  Available: keys, values, has_key, get, get_or, merge, entries, size\n");
}

} // namespace stdlib
} // namespace naab
