// NAAb Logging System Implementation

#include "naab/logger.h"

namespace naab {
namespace logging {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setLevel(Level level) {
    current_level_ = level;
}

Level Logger::getLevel() const {
    return current_level_;
}

void Logger::setVerbose(bool verbose) {
    if (verbose) {
        current_level_ = Level::TRACE;  // Show everything
    } else {
        current_level_ = Level::WARN;   // Show only warnings and errors
    }
}

bool Logger::shouldLog(Level level) const {
    return level >= current_level_;
}

} // namespace logging
} // namespace naab
