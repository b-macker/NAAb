#ifndef NAAB_LOGGER_H
#define NAAB_LOGGER_H

// NAAb Logging System
// Provides clean, configurable logging with verbosity levels

#include <fmt/core.h>
#include <string>
#include <utility>

namespace naab {
namespace logging {

// Log levels (ordered by severity)
enum class Level {
    TRACE = 0,   // Very detailed (e.g., every function call)
    DEBUG = 1,   // Debugging info (e.g., module loading)
    INFO = 2,    // General info (e.g., initialization)
    WARN = 3,    // Warnings
    ERROR = 4,   // Errors
    SILENT = 5   // No output
};

// Global logger configuration
class Logger {
public:
    // Get singleton instance
    static Logger& instance();

    // Set current log level
    void setLevel(Level level);

    // Get current log level
    Level getLevel() const;

    // Enable/disable verbose mode (sets level to TRACE)
    void setVerbose(bool verbose);

    // Check if a level should be logged
    bool shouldLog(Level level) const;

    // Log functions with level checking
    template<typename... Args>
    void trace(const std::string& format, Args&&... args) {
        if (shouldLog(Level::TRACE)) {
            fmt::print("[TRACE] " + format, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    void debug(const std::string& format, Args&&... args) {
        if (shouldLog(Level::DEBUG)) {
            fmt::print("[DEBUG] " + format, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    void info(const std::string& format, Args&&... args) {
        if (shouldLog(Level::INFO)) {
            fmt::print("[INFO] " + format, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    void warn(const std::string& format, Args&&... args) {
        if (shouldLog(Level::WARN)) {
            fmt::print("[WARN] " + format, std::forward<Args>(args)...);
        }
    }

    template<typename... Args>
    void error(const std::string& format, Args&&... args) {
        if (shouldLog(Level::ERROR)) {
            fmt::print("[ERROR] " + format, std::forward<Args>(args)...);
        }
    }

private:
    Logger() : current_level_(Level::WARN) {}  // Default: only warnings and errors
    Level current_level_;
};

// Convenience macros for logging
#define LOG_TRACE(...) ::naab::logging::Logger::instance().trace(__VA_ARGS__)
#define LOG_DEBUG(...) ::naab::logging::Logger::instance().debug(__VA_ARGS__)
#define LOG_INFO(...) ::naab::logging::Logger::instance().info(__VA_ARGS__)
#define LOG_WARN(...) ::naab::logging::Logger::instance().warn(__VA_ARGS__)
#define LOG_ERROR(...) ::naab::logging::Logger::instance().error(__VA_ARGS__)

} // namespace logging
} // namespace naab

#endif // NAAB_LOGGER_H
