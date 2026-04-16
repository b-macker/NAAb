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
        "keys", "values", "has_key", "get", "get_or"
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

    throw std::runtime_error("Unknown dict function: " + function_name + "\n\n"
        "  Available: keys, values, has_key, get, get_or\n");
}

} // namespace stdlib
} // namespace naab
