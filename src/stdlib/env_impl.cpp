//
// NAAb Standard Library - Environment Module
// Complete implementation with 10 environment functions
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/utils/string_utils.h"
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <algorithm>

extern char **environ;

namespace naab {
namespace stdlib {

// Forward declarations
static std::string getString(const std::shared_ptr<interpreter::Value>& val);
static int getInt(const std::shared_ptr<interpreter::Value>& val);
static double getDouble(const std::shared_ptr<interpreter::Value>& val);
static bool getBool(const std::shared_ptr<interpreter::Value>& val);
static std::shared_ptr<interpreter::Value> makeString(const std::string& s);
static std::shared_ptr<interpreter::Value> makeInt(int i);
static std::shared_ptr<interpreter::Value> makeDouble(double d);
static std::shared_ptr<interpreter::Value> makeBool(bool b);
static std::shared_ptr<interpreter::Value> makeMap(const std::unordered_map<std::string, std::string>& m);
static std::shared_ptr<interpreter::Value> makeNull();
static std::unordered_map<std::string, std::string> parseEnvFile(const std::string& content);

bool EnvModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "get", "set_var", "has", "delete_var", "get_all",
        "load_dotenv", "parse_env_file", "get_int", "get_float", "get_bool",
        "get_args"  // ISS-028: Command-line arguments access
    };
    return functions.count(name) > 0;
}

std::shared_ptr<interpreter::Value> EnvModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Function 1: get - Get environment variable
    if (function_name == "get") {
        if (args.size() < 1 || args.size() > 2) {
            throw std::runtime_error("get() takes 1 or 2 arguments (key, default?)");
        }
        std::string key = getString(args[0]);
        const char* value = std::getenv(key.c_str());

        if (value != nullptr) {
            return makeString(std::string(value));
        } else if (args.size() == 2) {
            return args[1];  // Return default
        } else {
            return makeString("");
        }
    }

    // Function 2: set_var - Set environment variable
    if (function_name == "set_var") {
        if (args.size() != 2) {
            throw std::runtime_error("set_var() takes exactly 2 arguments (key, value)");
        }
        std::string key = getString(args[0]);
        std::string value = getString(args[1]);

        setenv(key.c_str(), value.c_str(), 1);
        return makeNull();
    }

    // Function 3: has - Check if environment variable exists
    if (function_name == "has") {
        if (args.size() != 1) {
            throw std::runtime_error("has() takes exactly 1 argument");
        }
        std::string key = getString(args[0]);
        const char* value = std::getenv(key.c_str());
        return makeBool(value != nullptr);
    }

    // Function 4: delete_var - Delete environment variable
    if (function_name == "delete_var") {
        if (args.size() != 1) {
            throw std::runtime_error("delete_var() takes exactly 1 argument");
        }
        std::string key = getString(args[0]);
        unsetenv(key.c_str());
        return makeNull();
    }

    // Function 5: get_all - Get all environment variables
    if (function_name == "get_all") {
        if (args.size() != 0) {
            throw std::runtime_error("get_all() takes no arguments");
        }

        std::unordered_map<std::string, std::string> env_map;
        for (char **env = environ; *env != nullptr; env++) {
            std::string env_str(*env);
            size_t pos = env_str.find('=');
            if (pos != std::string::npos) {
                std::string key = env_str.substr(0, pos);
                std::string value = env_str.substr(pos + 1);
                env_map[key] = value;
            }
        }
        return makeMap(env_map);
    }

    // Function 6: load_dotenv - Load environment variables from .env file
    if (function_name == "load_dotenv") {
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

        // Set environment variables
        for (const auto& pair : env_vars) {
            setenv(pair.first.c_str(), pair.second.c_str(), 1);
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
            std::vector<std::shared_ptr<interpreter::Value>> args_list;
            args_list.reserve(script_args.size());

            for (const auto& arg : script_args) {
                args_list.push_back(makeString(arg));
            }

            return std::make_shared<interpreter::Value>(std::move(args_list));
        } else {
            // Return empty list if no provider set
            return std::make_shared<interpreter::Value>(
                std::vector<std::shared_ptr<interpreter::Value>>{}
            );
        }
    }

    // Common LLM mistakes
    if (function_name == "get_env" || function_name == "getenv" || function_name == "getEnv") {
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
        "get_bool", "get_args"
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
static std::string getString(const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([](auto&& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return arg;
        } else {
            throw std::runtime_error("Expected string value");
        }
    }, val->data);
}

// Note: getInt() and getDouble() helper functions removed (unused)

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

static std::shared_ptr<interpreter::Value> makeString(const std::string& s) {
    return std::make_shared<interpreter::Value>(s);
}

static std::shared_ptr<interpreter::Value> makeInt(int i) {
    return std::make_shared<interpreter::Value>(i);
}

static std::shared_ptr<interpreter::Value> makeDouble(double d) {
    return std::make_shared<interpreter::Value>(d);
}

static std::shared_ptr<interpreter::Value> makeBool(bool b) {
    return std::make_shared<interpreter::Value>(b);
}

static std::shared_ptr<interpreter::Value> makeMap(const std::unordered_map<std::string, std::string>& m) {
    std::unordered_map<std::string, std::shared_ptr<interpreter::Value>> result;
    for (const auto& pair : m) {
        result[pair.first] = makeString(pair.second);
    }
    return std::make_shared<interpreter::Value>(result);
}

static std::shared_ptr<interpreter::Value> makeNull() {
    return std::make_shared<interpreter::Value>();
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
