//
// NAAb Standard Library - Environment Module
// Complete implementation with 10 environment functions
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/sandbox.h"
#include "naab/platform.h"
#include "naab/utils/string_utils.h"
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <algorithm>

#ifndef _WIN32
extern char **environ;
#else
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace naab {
namespace stdlib {

// V-SC-002: NAAb-internal secrets must not be readable by user scripts.
static const std::unordered_set<std::string> NAAB_INTERNAL_ENV_VARS = {
    "NAAB_LOCK_KEY",
};
static bool isBlockedEnvVar(const std::string& name) {
    return NAAB_INTERNAL_ENV_VARS.count(name) > 0;
}

// V-RCE-001: Variables that can hijack loader/runtime behavior in polyglot subprocesses.
static const std::unordered_set<std::string> NAAB_DANGEROUS_ENV_VARS = {
    "LD_PRELOAD", "LD_LIBRARY_PATH", "DYLD_INSERT_LIBRARIES", "DYLD_LIBRARY_PATH",
    "PYTHONPATH", "PYTHONSTARTUP", "NODE_OPTIONS", "NODE_PATH",
    "PERL5LIB", "PERLLIB", "RUBYOPT", "RUBYLIB",
    "JAVA_TOOL_OPTIONS", "JDK_JAVA_OPTIONS", "_JAVA_OPTIONS",
};
static bool isDangerousEnvVar(const std::string& name) {
    return NAAB_DANGEROUS_ENV_VARS.count(name) > 0;
}

// Security: Check sandbox permissions for environment access
static void checkEnvSandbox(const std::string& operation, const std::string& var_name = "") {
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (!sandbox) return;  // No sandbox active — allow

    if (!sandbox->canAccessEnv(var_name)) {
        sandbox->logViolation("env." + operation, var_name.empty() ? "(all)" : var_name,
                              "SYS_ENV capability required");
        throw std::runtime_error(
            "Security: env." + operation + "() denied by sandbox\n\n"
            "  Variable: " + (var_name.empty() ? "(all environment variables)" : var_name) + "\n\n"
            "  The current sandbox level does not permit environment access.\n"
            "  To allow, use: --sandbox-level standard (or higher)\n"
        );
    }
}

// Forward declarations
static std::string getString(const interpreter::NaabVal& val);
static int getInt(const interpreter::NaabVal& val);
static double getDouble(const interpreter::NaabVal& val);
static bool getBool(const interpreter::NaabVal& val);
static interpreter::NaabVal makeString(const std::string& s);
static interpreter::NaabVal makeInt(int i);
static interpreter::NaabVal makeDouble(double d);
static interpreter::NaabVal makeBool(bool b);
static interpreter::NaabVal makeMap(const std::unordered_map<std::string, std::string>& m);
static interpreter::NaabVal makeNull();
static std::unordered_map<std::string, std::string> parseEnvFile(const std::string& content);

bool EnvModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "get", "set_var", "has", "delete_var", "get_all",
        "load_dotenv", "parse_env_file", "get_int", "get_float", "get_bool",
        "get_args",  // ISS-028: Command-line arguments access
        "list"
    };
    return functions.count(name) > 0;
}

interpreter::NaabVal EnvModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    // Function 1: get - Get environment variable
    if (function_name == "get") {
        if (args.size() < 1 || args.size() > 2) {
            throw std::runtime_error("get() takes 1 or 2 arguments (key, default?)");
        }
        std::string key = getString(args[0]);
        checkEnvSandbox("get", key);
        if (isBlockedEnvVar(key)) {
            if (args.size() == 2) return args[1];
            return interpreter::NaabVal::makeNull();
        }
        const char* value = std::getenv(key.c_str());

        if (value != nullptr) {
            return makeString(std::string(value));
        } else if (args.size() == 2) {
            return args[1];  // Return default
        } else {
            return interpreter::NaabVal::makeNull();
        }
    }

    // Function 2: set_var - Set environment variable
    if (function_name == "set_var") {
        if (args.size() != 2) {
            throw std::runtime_error("set_var() takes exactly 2 arguments (key, value)");
        }
        std::string key = getString(args[0]);
        checkEnvSandbox("set_var", key);
        // V-ENV-001: block mutation of internal secrets
        if (isBlockedEnvVar(key)) {
            throw std::runtime_error(
                "Security: env.set_var() denied — '" + key + "' is an internal NAAb variable");
        }
        // V-RCE-001: block loader/runtime hijack variables
        if (isDangerousEnvVar(key)) {
            throw std::runtime_error(
                "Security: env.set_var() denied — '" + key + "' can hijack polyglot sandbox");
        }
        std::string value = getString(args[1]);

        naab::platform::setenv(key, value, true);
        return makeNull();
    }

    // Function 3: has - Check if environment variable exists
    if (function_name == "has") {
        if (args.size() != 1) {
            throw std::runtime_error("has() takes exactly 1 argument");
        }
        std::string key = getString(args[0]);
        checkEnvSandbox("has", key);
        if (isBlockedEnvVar(key)) return makeBool(false);
        const char* value = std::getenv(key.c_str());
        return makeBool(value != nullptr);
    }

    // Function 4: delete_var - Delete environment variable
    if (function_name == "delete_var") {
        if (args.size() != 1) {
            throw std::runtime_error("delete_var() takes exactly 1 argument");
        }
        std::string key = getString(args[0]);
        checkEnvSandbox("delete_var", key);
        // V-ENV-001: block deletion of internal secrets
        if (isBlockedEnvVar(key)) {
            throw std::runtime_error(
                "Security: env.delete_var() denied — '" + key + "' is an internal NAAb variable");
        }
        naab::platform::unsetenv(key);
        return makeNull();
    }

    // Function 5: get_all / list - Get all environment variables
    if (function_name == "get_all" || function_name == "list") {
        if (args.size() != 0) {
            throw std::runtime_error("get_all() takes no arguments");
        }
        checkEnvSandbox("get_all");

        std::unordered_map<std::string, std::string> env_map;
#ifndef _WIN32
        for (char **ep = environ; *ep != nullptr; ep++) {
            std::string env_str(*ep);
            size_t pos = env_str.find('=');
            if (pos != std::string::npos) {
                env_map[env_str.substr(0, pos)] = env_str.substr(pos + 1);
            }
        }
#else
        // On Windows, enumerate the process environment block
        LPCH env_block = ::GetEnvironmentStringsA();
        if (env_block) {
            for (LPCH p = env_block; *p; ) {
                std::string entry(p);
                p += entry.size() + 1;
                size_t pos = entry.find('=');
                if (pos != std::string::npos && pos > 0) {
                    env_map[entry.substr(0, pos)] = entry.substr(pos + 1);
                }
            }
            ::FreeEnvironmentStringsA(env_block);
        }
#endif
        // V-SC-002: strip NAAb-internal secrets before returning to user script
        for (const auto& blocked : NAAB_INTERNAL_ENV_VARS) {
            env_map.erase(blocked);
        }
        return makeMap(env_map);
    }

    // Function 6: load_dotenv - Load environment variables from .env file
    if (function_name == "load_dotenv") {
        checkEnvSandbox("load_dotenv");
        std::string path = ".env";
        bool strict = false;  // Default: lenient mode

        if (args.size() >= 1) {
            path = getString(args[0]);
        }
        if (args.size() >= 2) {
            strict = getBool(args[1]);
        }
        if (args.size() > 2) {
            throw std::runtime_error("load_dotenv() takes 0-2 arguments (path?, strict?)");
        }

        std::ifstream file(path);
        if (!file.is_open()) {
            if (strict) {
                throw std::runtime_error("load_dotenv() failed to open file: " + path);
            }
            // In lenient mode, return empty map if file doesn't exist
            return makeMap(std::unordered_map<std::string, std::string>());
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        auto env_vars = parseEnvFile(content);

        // V-ENV-001 + V-RCE-001: strip internal and dangerous vars loaded from .env files
        for (auto it = env_vars.begin(); it != env_vars.end(); ) {
            if (isBlockedEnvVar(it->first) || isDangerousEnvVar(it->first)) {
                fprintf(stderr, "[env] Warning: load_dotenv skipping blocked variable: %s\n",
                        it->first.c_str());
                it = env_vars.erase(it);
            } else {
                ++it;
            }
        }

        // Set environment variables
        for (const auto& pair : env_vars) {
            naab::platform::setenv(pair.first, pair.second, true);
        }

        return makeMap(env_vars);
    }

    // Function 7: parse_env_file - Parse .env file content
    if (function_name == "parse_env_file") {
        if (args.size() != 1) {
            throw std::runtime_error("parse_env_file() takes exactly 1 argument (content)");
        }
        std::string content = getString(args[0]);
        auto env_vars = parseEnvFile(content);
        return makeMap(env_vars);
    }

    // Function 8: get_int - Get environment variable as integer
    if (function_name == "get_int") {
        if (args.size() < 1 || args.size() > 2) {
            throw std::runtime_error("get_int() takes 1 or 2 arguments (key, default?)");
        }
        std::string key = getString(args[0]);
        checkEnvSandbox("get_int", key);
        if (isBlockedEnvVar(key)) {
            if (args.size() == 2) return args[1];
            return makeInt(0);
        }
        const char* value = std::getenv(key.c_str());

        if (value != nullptr) {
            try {
                return makeInt(std::stoi(std::string(value)));
            } catch (...) {
                throw std::runtime_error("Failed to parse environment variable as integer");
            }
        } else if (args.size() == 2) {
            return args[1];  // Return default
        } else {
            return makeInt(0);
        }
    }

    // Function 9: get_float - Get environment variable as float
    if (function_name == "get_float") {
        if (args.size() < 1 || args.size() > 2) {
            throw std::runtime_error("get_float() takes 1 or 2 arguments (key, default?)");
        }
        std::string key = getString(args[0]);
        checkEnvSandbox("get_float", key);
        if (isBlockedEnvVar(key)) {
            if (args.size() == 2) return args[1];
            return makeDouble(0.0);
        }
        const char* value = std::getenv(key.c_str());

        if (value != nullptr) {
            try {
                return makeDouble(std::stod(std::string(value)));
            } catch (...) {
                throw std::runtime_error("Failed to parse environment variable as float");
            }
        } else if (args.size() == 2) {
            return args[1];  // Return default
        } else {
            return makeDouble(0.0);
        }
    }

    // Function 10: get_bool - Get environment variable as boolean
    if (function_name == "get_bool") {
        if (args.size() < 1 || args.size() > 2) {
            throw std::runtime_error("get_bool() takes 1 or 2 arguments (key, default?)");
        }
        std::string key = getString(args[0]);
        checkEnvSandbox("get_bool", key);
        if (isBlockedEnvVar(key)) {
            if (args.size() == 2) return args[1];
            return makeBool(false);
        }
        const char* value = std::getenv(key.c_str());

        if (value != nullptr) {
            std::string val_str(value);
            std::transform(val_str.begin(), val_str.end(), val_str.begin(), ::tolower);

            if (val_str == "true" || val_str == "1" || val_str == "yes" || val_str == "on") {
                return makeBool(true);
            } else if (val_str == "false" || val_str == "0" || val_str == "no" || val_str == "off") {
                return makeBool(false);
            } else {
                throw std::runtime_error("Invalid boolean value in environment variable");
            }
        } else if (args.size() == 2) {
            return args[1];  // Return default
        } else {
            return makeBool(false);
        }
    }

    // Function 11: get_args - Get command-line arguments (ISS-028)
    if (function_name == "get_args") {
        if (!args.empty()) {
            throw std::runtime_error("get_args() takes no arguments");
        }

        // Use args provider callback if available
        if (args_provider_) {
            std::vector<std::string> script_args = args_provider_();
            std::vector<interpreter::NaabVal> args_list;
            args_list.reserve(script_args.size());

            for (const auto& arg : script_args) {
                args_list.push_back(makeString(arg));
            }

            return interpreter::NaabVal::makeList(std::move(args_list));
        } else {
            // Return empty list if no provider set
            return interpreter::NaabVal::makeList(std::vector<interpreter::NaabVal>{});
        }
    }

    // Common LLM mistakes
    if (function_name == "get_env" || function_name == "getenv" || function_name == "getEnv" ||
        function_name == "get_var" || function_name == "getVar") {
        throw std::runtime_error(
            "Unknown env function: " + function_name + "\n\n"
            "  Did you mean: env.get()?\n"
            "  Example: let val = env.get(\"HOME\")\n"
        );
    }
    if (function_name == "set" || function_name == "setenv" || function_name == "setEnv" || function_name == "put") {
        throw std::runtime_error(
            "Unknown env function: " + function_name + "\n\n"
            "  Did you mean: env.set_var()?\n"
            "  Example: env.set_var(\"MY_KEY\", \"my_value\")\n"
        );
    }

    // Fuzzy matching for typos
    static const std::vector<std::string> FUNCTIONS = {
        "get", "set_var", "has", "delete_var", "get_all",
        "load_dotenv", "parse_env_file", "get_int", "get_float",
        "get_bool", "get_args", "list"
    };
    auto similar = naab::utils::findSimilar(function_name, FUNCTIONS);
    std::string suggestion = naab::utils::formatSuggestions(function_name, similar);

    std::ostringstream oss;
    oss << "Unknown env function: " << function_name << suggestion
        << "\n\n  Available: ";
    for (size_t i = 0; i < FUNCTIONS.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << FUNCTIONS[i];
    }
    throw std::runtime_error(oss.str());
}

// Helper functions
static std::string getString(const interpreter::NaabVal& val) {
    if (!val.isString()) throw std::runtime_error("Expected string value");
    return val.asString();
}

static bool getBool(const interpreter::NaabVal& val) {
    if (!val.isBool()) throw std::runtime_error("Expected boolean value");
    return val.asBool();
}

static interpreter::NaabVal makeString(const std::string& s) {
    return interpreter::NaabVal::makeString(s);
}

static interpreter::NaabVal makeInt(int i) {
    return interpreter::NaabVal::makeInt(i);
}

static interpreter::NaabVal makeDouble(double d) {
    return interpreter::NaabVal::makeDouble(d);
}

static interpreter::NaabVal makeBool(bool b) {
    return interpreter::NaabVal::makeBool(b);
}

static interpreter::NaabVal makeMap(const std::unordered_map<std::string, std::string>& m) {
    std::unordered_map<std::string, interpreter::NaabVal> result;
    for (const auto& pair : m) {
        result[pair.first] = makeString(pair.second);
    }
    return interpreter::NaabVal::makeDict(std::move(result));
}

static interpreter::NaabVal makeNull() {
    return interpreter::NaabVal::makeNull();
}

static std::unordered_map<std::string, std::string> parseEnvFile(const std::string& content) {
    std::unordered_map<std::string, std::string> result;
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Parse KEY=VALUE
        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);

            // Trim key and value
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            // Remove quotes if present
            if (value.size() >= 2 &&
                ((value.front() == '"' && value.back() == '"') ||
                 (value.front() == '\'' && value.back() == '\''))) {
                value = value.substr(1, value.size() - 2);
            }

            result[key] = value;
        }
    }

    return result;
}

} // namespace stdlib
} // namespace naab
