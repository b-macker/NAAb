//
// NAAb Standard Library - Time Module
// Complete implementation with 13 time functions
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/utils/string_utils.h"
#include <chrono>
#include <ctime>
#include <thread>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace naab {
namespace stdlib {

// Forward declarations
static int getInt(const interpreter::NaabVal& val);
static int64_t getTimestamp(const interpreter::NaabVal& val);
static double getDouble(const interpreter::NaabVal& val);
static std::string getString(const interpreter::NaabVal& val);
static interpreter::NaabVal makeInt(int i);
static interpreter::NaabVal makeString(const std::string& s);
static interpreter::NaabVal makeNull();

bool TimeModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "now", "now_millis", "sleep", "format_timestamp", "parse_datetime",
        "year", "month", "day", "hour", "minute", "second", "weekday"
    };
    return functions.count(name) > 0;
}

interpreter::NaabVal TimeModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    // Function 1: now - Unix timestamp in seconds
    if (function_name == "now") {
        if (args.size() != 0) {
            throw std::runtime_error("now() takes no arguments");
        }
        auto now = std::chrono::system_clock::now();
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        // Return fractional seconds (millisecond precision) as double
        return interpreter::NaabVal::makeDouble(static_cast<double>(millis) / 1000.0);
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
        return interpreter::NaabVal::makeDouble(static_cast<double>(millis));
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
        if (args.size() == 1) {
            throw std::runtime_error(
                "format_timestamp() requires 2 arguments: (timestamp, format_string)\n\n"
                "  Got: time.format_timestamp(timestamp)\n"
                "  Expected: time.format_timestamp(timestamp, format_string)\n\n"
                "  Common format strings:\n"
                "    \"%Y-%m-%d %H:%M:%S\"  →  2026-04-24 14:30:00\n"
                "    \"%Y-%m-%d\"            →  2026-04-24\n"
                "    \"%H:%M:%S\"            →  14:30:00\n\n"
                "  Example:\n"
                "    time.format_timestamp(time.now(), \"%Y-%m-%d %H:%M:%S\")\n"
            );
        }
        if (args.size() != 2) {
            throw std::runtime_error("format_timestamp() takes exactly 2 arguments (timestamp, format_string)");
        }
        int64_t timestamp = getTimestamp(args[0]);
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
        return interpreter::NaabVal::makeDouble(static_cast<double>(time));
    }

    // Function 6: year - Get year from timestamp (or current)
    if (function_name == "year") {
        int64_t timestamp;
        if (args.size() == 0) {
            auto now = std::chrono::system_clock::now();
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        } else if (args.size() == 1) {
            timestamp = getTimestamp(args[0]);
        } else {
            throw std::runtime_error("year() takes 0 or 1 argument");
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);
        return makeInt(tm_info->tm_year + 1900);
    }

    // Function 7: month - Get month from timestamp (or current)
    if (function_name == "month") {
        int64_t timestamp;
        if (args.size() == 0) {
            auto now = std::chrono::system_clock::now();
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        } else if (args.size() == 1) {
            timestamp = getTimestamp(args[0]);
        } else {
            throw std::runtime_error("month() takes 0 or 1 argument");
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);
        return makeInt(tm_info->tm_mon + 1);  // 1-12 instead of 0-11
    }

    // Function 8: day - Get day from timestamp (or current)
    if (function_name == "day") {
        int64_t timestamp;
        if (args.size() == 0) {
            auto now = std::chrono::system_clock::now();
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        } else if (args.size() == 1) {
            timestamp = getTimestamp(args[0]);
        } else {
            throw std::runtime_error("day() takes 0 or 1 argument");
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);
        return makeInt(tm_info->tm_mday);
    }

    // Function 9: hour - Get hour from timestamp (or current)
    if (function_name == "hour") {
        int64_t timestamp;
        if (args.size() == 0) {
            auto now = std::chrono::system_clock::now();
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        } else if (args.size() == 1) {
            timestamp = getTimestamp(args[0]);
        } else {
            throw std::runtime_error("hour() takes 0 or 1 argument");
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);
        return makeInt(tm_info->tm_hour);
    }

    // Function 10: minute - Get minute from timestamp (or current)
    if (function_name == "minute") {
        int64_t timestamp;
        if (args.size() == 0) {
            auto now = std::chrono::system_clock::now();
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        } else if (args.size() == 1) {
            timestamp = getTimestamp(args[0]);
        } else {
            throw std::runtime_error("minute() takes 0 or 1 argument");
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);
        return makeInt(tm_info->tm_min);
    }

    // Function 11: second - Get second from timestamp (or current)
    if (function_name == "second") {
        int64_t timestamp;
        if (args.size() == 0) {
            auto now = std::chrono::system_clock::now();
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        } else if (args.size() == 1) {
            timestamp = getTimestamp(args[0]);
        } else {
            throw std::runtime_error("second() takes 0 or 1 argument");
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);
        return makeInt(tm_info->tm_sec);
    }

    // Function 12: weekday - Get weekday from timestamp (or current) (0=Sunday, 6=Saturday)
    if (function_name == "weekday") {
        int64_t timestamp;
        if (args.size() == 0) {
            auto now = std::chrono::system_clock::now();
            timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
        } else if (args.size() == 1) {
            timestamp = getTimestamp(args[0]);
        } else {
            throw std::runtime_error("weekday() takes 0 or 1 argument");
        }

        std::time_t time = static_cast<std::time_t>(timestamp);
        std::tm* tm_info = std::localtime(&time);
        return makeInt(tm_info->tm_wday);
    }

    // Common LLM mistakes
    if (function_name == "now_ms") {
        throw std::runtime_error(
            "Unknown time function: " + function_name + "\n\n"
            "  Did you mean: time.now_millis()?\n"
        );
    }
    if (function_name == "format" || function_name == "strftime") {
        throw std::runtime_error(
            "Unknown time function: " + function_name + "\n\n"
            "  Did you mean: time.format_timestamp(timestamp, format_string)?\n"
            "  Example: time.format_timestamp(time.now(), \"%Y-%m-%d %H:%M:%S\")\n"
        );
    }
    if (function_name == "parse" || function_name == "strptime") {
        throw std::runtime_error(
            "Unknown time function: " + function_name + "\n\n"
            "  Did you mean: time.parse_datetime(datetime_string, format_string)?\n"
            "  Example: time.parse_datetime(\"2024-01-15 10:00:00\", \"%Y-%m-%d %H:%M:%S\")\n"
        );
    }
    if (function_name == "current" || function_name == "timestamp" || function_name == "get_time") {
        throw std::runtime_error(
            "Unknown time function: " + function_name + "\n\n"
            "  Did you mean: time.now()?\n"
            "  Example: let t = time.now()\n"
        );
    }
    if (function_name == "wait" || function_name == "delay" || function_name == "pause") {
        throw std::runtime_error(
            "Unknown time function: " + function_name + "\n\n"
            "  Did you mean: time.sleep()?\n"
            "  Example: time.sleep(1.0)  // sleep for 1 second\n"
        );
    }

    // Fuzzy matching for typos
    static const std::vector<std::string> FUNCTIONS = {
        "now", "now_millis", "sleep", "format_timestamp", "parse_datetime",
        "year", "month", "day", "hour", "minute", "second", "weekday"
    };
    auto similar = naab::utils::findSimilar(function_name, FUNCTIONS);
    std::string suggestion = naab::utils::formatSuggestions(function_name, similar);

    std::ostringstream oss;
    oss << "Unknown time function: " << function_name << suggestion
        << "\n\n  Available: ";
    for (size_t i = 0; i < FUNCTIONS.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << FUNCTIONS[i];
    }
    throw std::runtime_error(oss.str());
}

// Helper functions
static int getInt(const interpreter::NaabVal& val) {
    if (val.isInt()) return val.asInt();
    if (val.isDouble()) return static_cast<int>(val.asDouble());
    throw std::runtime_error("Expected integer value");
}

// 64-bit timestamp extraction — accepts int or double (from time.now())
static int64_t getTimestamp(const interpreter::NaabVal& val) {
    if (val.isInt()) return static_cast<int64_t>(val.asInt());
    if (val.isDouble()) return static_cast<int64_t>(val.asDouble());
    throw std::runtime_error("Expected numeric timestamp value");
}

static double getDouble(const interpreter::NaabVal& val) {
    if (val.isDouble()) return val.asDouble();
    if (val.isInt()) return static_cast<double>(val.asInt());
    throw std::runtime_error("Expected numeric value");
}

static std::string getString(const interpreter::NaabVal& val) {
    if (!val.isString()) throw std::runtime_error("Expected string value");
    return val.asString();
}

static interpreter::NaabVal makeInt(int i) {
    return interpreter::NaabVal::makeInt(i);
}

static interpreter::NaabVal makeString(const std::string& s) {
    return interpreter::NaabVal::makeString(s);
}

static interpreter::NaabVal makeNull() {
    return interpreter::NaabVal::makeNull();
}

} // namespace stdlib
} // namespace naab
