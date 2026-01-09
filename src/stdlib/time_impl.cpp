//
// NAAb Standard Library - Time Module
// Complete implementation with 13 time functions
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include <chrono>
#include <ctime>
#include <thread>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace naab {
namespace stdlib {

// Forward declarations
static int getInt(const std::shared_ptr<interpreter::Value>& val);
static double getDouble(const std::shared_ptr<interpreter::Value>& val);
static std::string getString(const std::shared_ptr<interpreter::Value>& val);
static std::shared_ptr<interpreter::Value> makeInt(int i);
static std::shared_ptr<interpreter::Value> makeString(const std::string& s);
static std::shared_ptr<interpreter::Value> makeNull();

bool TimeModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "now", "now_millis", "sleep", "format_timestamp", "parse_datetime",
        "year", "month", "day", "hour", "minute", "second", "weekday"
    };
    return functions.count(name) > 0;
}

std::shared_ptr<interpreter::Value> TimeModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Function 1: now - Unix timestamp in seconds
    if (function_name == "now") {
        if (args.size() != 0) {
            throw std::runtime_error("now() takes no arguments");
        }
        auto now = std::chrono::system_clock::now();
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();
        // Return double to avoid 2038 overflow problem
        return std::make_shared<interpreter::Value>(static_cast<double>(seconds));
    }

    // Function 2: now_millis - Unix timestamp in milliseconds
    if (function_name == "now_millis") {
        if (args.size() != 0) {
            throw std::runtime_error("now_millis() takes no arguments");
        }
        auto now = std::chrono::system_clock::now();
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        // Return double to avoid overflow (int maxes out at ~24.8 days worth of milliseconds)
        return std::make_shared<interpreter::Value>(static_cast<double>(millis));
    }

    // Function 3: sleep - Sleep for specified seconds
    if (function_name == "sleep") {
        if (args.size() != 1) {
            throw std::runtime_error("sleep() takes exactly 1 argument");
        }
        double seconds = getDouble(args[0]);
        std::this_thread::sleep_for(
            std::chrono::duration<double>(seconds));
        return makeNull();
    }

    // Function 4: format_timestamp - Format timestamp as string
    if (function_name == "format_timestamp") {
        if (args.size() != 2) {
            throw std::runtime_error("format_timestamp() takes exactly 2 arguments (timestamp, format)");
        }
        int timestamp = getInt(args[0]);
        std::string format = getString(args[1]);

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);

        std::ostringstream oss;
        oss << std::put_time(tm_info, format.c_str());
        return makeString(oss.str());
    }

    // Function 5: parse_datetime - Parse datetime string to timestamp
    if (function_name == "parse_datetime") {
        if (args.size() != 2) {
            throw std::runtime_error("parse_datetime() takes exactly 2 arguments (date_str, format)");
        }
        std::string date_str = getString(args[0]);
        std::string format = getString(args[1]);

        std::tm tm_info = {};
        std::istringstream iss(date_str);
        iss >> std::get_time(&tm_info, format.c_str());

        // Enhanced validation with informative error message
        if (iss.fail()) {
            throw std::runtime_error("parse_datetime() failed to parse '" + date_str +
                                   "' with format '" + format + "'");
        }

        std::time_t time = std::mktime(&tm_info);
        return makeInt(static_cast<int>(time));
    }

    // Function 6: year - Get year from timestamp (or current)
    if (function_name == "year") {
        int timestamp;
        if (args.size() == 0) {
            auto now = std::chrono::system_clock::now();
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        } else if (args.size() == 1) {
            timestamp = getInt(args[0]);
        } else {
            throw std::runtime_error("year() takes 0 or 1 argument");
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);
        return makeInt(tm_info->tm_year + 1900);
    }

    // Function 7: month - Get month from timestamp (or current)
    if (function_name == "month") {
        int timestamp;
        if (args.size() == 0) {
            auto now = std::chrono::system_clock::now();
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        } else if (args.size() == 1) {
            timestamp = getInt(args[0]);
        } else {
            throw std::runtime_error("month() takes 0 or 1 argument");
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);
        return makeInt(tm_info->tm_mon + 1);  // 1-12 instead of 0-11
    }

    // Function 8: day - Get day from timestamp (or current)
    if (function_name == "day") {
        int timestamp;
        if (args.size() == 0) {
            auto now = std::chrono::system_clock::now();
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        } else if (args.size() == 1) {
            timestamp = getInt(args[0]);
        } else {
            throw std::runtime_error("day() takes 0 or 1 argument");
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);
        return makeInt(tm_info->tm_mday);
    }

    // Function 9: hour - Get hour from timestamp (or current)
    if (function_name == "hour") {
        int timestamp;
        if (args.size() == 0) {
            auto now = std::chrono::system_clock::now();
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        } else if (args.size() == 1) {
            timestamp = getInt(args[0]);
        } else {
            throw std::runtime_error("hour() takes 0 or 1 argument");
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);
        return makeInt(tm_info->tm_hour);
    }

    // Function 10: minute - Get minute from timestamp (or current)
    if (function_name == "minute") {
        int timestamp;
        if (args.size() == 0) {
            auto now = std::chrono::system_clock::now();
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        } else if (args.size() == 1) {
            timestamp = getInt(args[0]);
        } else {
            throw std::runtime_error("minute() takes 0 or 1 argument");
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);
        return makeInt(tm_info->tm_min);
    }

    // Function 11: second - Get second from timestamp (or current)
    if (function_name == "second") {
        int timestamp;
        if (args.size() == 0) {
            auto now = std::chrono::system_clock::now();
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        } else if (args.size() == 1) {
            timestamp = getInt(args[0]);
        } else {
            throw std::runtime_error("second() takes 0 or 1 argument");
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);
        return makeInt(tm_info->tm_sec);
    }

    // Function 12: weekday - Get weekday from timestamp (or current) (0=Sunday, 6=Saturday)
    if (function_name == "weekday") {
        int timestamp;
        if (args.size() == 0) {
            auto now = std::chrono::system_clock::now();
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        } else if (args.size() == 1) {
            timestamp = getInt(args[0]);
        } else {
            throw std::runtime_error("weekday() takes 0 or 1 argument");
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);
        return makeInt(tm_info->tm_wday);
    }

    throw std::runtime_error("Unknown function: " + function_name);
}

// Helper functions
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

static std::shared_ptr<interpreter::Value> makeInt(int i) {
    return std::make_shared<interpreter::Value>(i);
}

static std::shared_ptr<interpreter::Value> makeString(const std::string& s) {
    return std::make_shared<interpreter::Value>(s);
}

static std::shared_ptr<interpreter::Value> makeNull() {
    return std::make_shared<interpreter::Value>();
}

} // namespace stdlib
} // namespace naab
