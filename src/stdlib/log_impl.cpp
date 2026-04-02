//
// NAAb Standard Library - Log Module
// Structured logging with levels, formats, and output targets
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include <chrono>
#include <ctime>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace naab {
namespace stdlib {

// Level ordering: debug(0) < info(1) < warn(2) < error(3) < none(4)
static int levelToInt(const std::string& level) {
    if (level == "debug") return 0;
    if (level == "info")  return 1;
    if (level == "warn")  return 2;
    if (level == "error") return 3;
    if (level == "none")  return 4;
    return 1; // default to info
}

static std::string getISO8601() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%FT%TZ");
    return oss.str();
}

// Escape double-quotes in a string for JSON embedding
static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else                out += c;
    }
    return out;
}

bool LogModule::shouldLog(const std::string& msg_level) const {
    return levelToInt(msg_level) >= levelToInt(level_);
}

std::ostream& LogModule::getStream() {
    if (output_ == "stderr") return std::cerr;
    if (output_ == "stdout") return std::cout;
    // File output — lazily opened
    if (!file_stream_ || !file_stream_->is_open()) {
        file_stream_ = std::make_shared<std::ofstream>(output_, std::ios::app);
        if (!file_stream_->is_open()) {
            throw std::runtime_error("log.set_output: cannot open file: " + output_);
        }
    }
    return *file_stream_;
}

void LogModule::writeLog(const std::string& level, const std::string& msg) {
    if (!shouldLog(level)) return;
    auto& out = getStream();
    if (format_ == "json") {
        out << "{\"level\":\"" << level << "\",\"msg\":\"" << jsonEscape(msg)
            << "\",\"ts\":\"" << getISO8601() << "\"}\n";
    } else {
        // text format: [LEVEL] message
        std::string upper_level = level;
        for (char& c : upper_level) c = static_cast<char>(::toupper(c));
        out << "[" << upper_level << "] " << msg << "\n";
    }
    out.flush();
}

bool LogModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "info", "warn", "error", "debug", "log",
        "set_level", "set_format", "set_output", "get_level"
    };
    return functions.count(name) > 0;
}

interpreter::NaabVal LogModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    if (function_name == "info" || function_name == "warn" ||
        function_name == "error" || function_name == "debug") {
        if (args.size() != 1) {
            throw std::runtime_error("log." + function_name + "() takes exactly 1 argument");
        }
        writeLog(function_name, args[0].toString());
        return interpreter::NaabVal::makeNull();
    }

    if (function_name == "log") {
        if (args.size() != 2) {
            throw std::runtime_error(
                "log.log() takes exactly 2 arguments: (level, message)\n\n"
                "  Example: log.log(\"warn\", \"something happened\")\n"
            );
        }
        std::string lvl = args[0].toString();
        if (lvl != "debug" && lvl != "info" && lvl != "warn" && lvl != "error") {
            throw std::runtime_error(
                "log.log(): invalid level \"" + lvl + "\"\n\n"
                "  Valid levels: debug, info, warn, error\n"
            );
        }
        writeLog(lvl, args[1].toString());
        return interpreter::NaabVal::makeNull();
    }

    if (function_name == "set_level") {
        if (args.size() != 1) {
            throw std::runtime_error("log.set_level() takes exactly 1 argument");
        }
        std::string lvl = args[0].toString();
        if (lvl != "debug" && lvl != "info" && lvl != "warn" &&
            lvl != "error" && lvl != "none") {
            throw std::runtime_error(
                "log.set_level(): invalid level \"" + lvl + "\"\n\n"
                "  Valid levels: debug, info, warn, error, none\n"
            );
        }
        level_ = lvl;
        return interpreter::NaabVal::makeNull();
    }

    if (function_name == "get_level") {
        return interpreter::NaabVal::makeString(level_);
    }

    if (function_name == "set_format") {
        if (args.size() != 1) {
            throw std::runtime_error("log.set_format() takes exactly 1 argument");
        }
        std::string fmt = args[0].toString();
        if (fmt != "text" && fmt != "json") {
            throw std::runtime_error(
                "log.set_format(): invalid format \"" + fmt + "\"\n\n"
                "  Valid formats: text, json\n"
            );
        }
        format_ = fmt;
        return interpreter::NaabVal::makeNull();
    }

    if (function_name == "set_output") {
        if (args.size() != 1) {
            throw std::runtime_error("log.set_output() takes exactly 1 argument");
        }
        std::string target = args[0].toString();
        // Close existing file stream if switching away from file
        if (output_ != "stderr" && output_ != "stdout" && file_stream_) {
            file_stream_->close();
            file_stream_.reset();
        }
        output_ = target;
        // If switching to a new file, reset stream so it gets re-opened lazily
        if (target != "stderr" && target != "stdout") {
            file_stream_.reset();
        }
        return interpreter::NaabVal::makeNull();
    }

    throw std::runtime_error(
        "Unknown log function: " + function_name + "\n\n"
        "  Available: info, warn, error, debug, log, set_level, get_level, set_format, set_output\n"
    );
}

} // namespace stdlib
} // namespace naab
