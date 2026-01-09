#include "naab/audit_logger.h"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

namespace naab {
namespace security {

// Static member initialization
std::string AuditLogger::log_file_path_ = "";
size_t AuditLogger::max_file_size_ = 10 * 1024 * 1024;  // 10 MB default
bool AuditLogger::enabled_ = true;
std::mutex AuditLogger::mutex_;
std::ofstream AuditLogger::log_stream_;

void AuditLogger::log(AuditEvent event, const std::string& details) {
    logWithMetadata(event, details, {});
}

void AuditLogger::logWithMetadata(AuditEvent event, const std::string& details,
                                   const std::map<std::string, std::string>& metadata) {
    if (!enabled_) {
        return;
    }

    AuditLogEntry entry;
    entry.timestamp = getCurrentTimestamp();
    entry.event = event;
    entry.details = details;
    entry.metadata = metadata;

    std::string json = formatLogEntry(entry);
    writeLogEntry(json);
}

void AuditLogger::logBlockLoad(const std::string& block_id, const std::string& hash) {
    std::map<std::string, std::string> metadata;
    metadata["block_id"] = block_id;
    metadata["hash"] = hash;
    logWithMetadata(AuditEvent::BLOCK_LOAD, "Block loaded successfully", metadata);
}

void AuditLogger::logBlockExecute(const std::string& block_id, const std::string& language) {
    std::map<std::string, std::string> metadata;
    metadata["block_id"] = block_id;
    metadata["language"] = language;
    logWithMetadata(AuditEvent::BLOCK_EXECUTE, "Block executed", metadata);
}

void AuditLogger::logSecurityViolation(const std::string& reason) {
    log(AuditEvent::SECURITY_VIOLATION, reason);
}

void AuditLogger::logTimeout(const std::string& operation, unsigned int timeout_seconds) {
    std::map<std::string, std::string> metadata;
    metadata["operation"] = operation;
    metadata["timeout_seconds"] = std::to_string(timeout_seconds);
    logWithMetadata(AuditEvent::TIMEOUT, "Operation timed out", metadata);
}

void AuditLogger::logInvalidPath(const std::string& path, const std::string& reason) {
    std::map<std::string, std::string> metadata;
    metadata["path"] = path;
    metadata["reason"] = reason;
    logWithMetadata(AuditEvent::INVALID_PATH, "Invalid path detected", metadata);
}

void AuditLogger::logHashMismatch(const std::string& block_id, const std::string& expected,
                                   const std::string& actual) {
    std::map<std::string, std::string> metadata;
    metadata["block_id"] = block_id;
    metadata["expected_hash"] = expected;
    metadata["actual_hash"] = actual;
    logWithMetadata(AuditEvent::HASH_MISMATCH, "Code integrity check failed", metadata);
}

void AuditLogger::setLogFile(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_file_path_ = filepath;

    // Close existing stream if open
    if (log_stream_.is_open()) {
        log_stream_.close();
    }
}

void AuditLogger::setMaxFileSize(size_t max_size_bytes) {
    max_file_size_ = max_size_bytes;
}

void AuditLogger::setEnabled(bool enabled) {
    enabled_ = enabled;
}

void AuditLogger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_stream_.is_open()) {
        log_stream_.flush();
    }
}

std::string AuditLogger::getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';

    return oss.str();
}

std::string AuditLogger::eventToString(AuditEvent event) {
    switch (event) {
        case AuditEvent::BLOCK_LOAD:
            return "BLOCK_LOAD";
        case AuditEvent::BLOCK_EXECUTE:
            return "BLOCK_EXECUTE";
        case AuditEvent::SECURITY_VIOLATION:
            return "SECURITY_VIOLATION";
        case AuditEvent::TIMEOUT:
            return "TIMEOUT";
        case AuditEvent::INVALID_PATH:
            return "INVALID_PATH";
        case AuditEvent::INVALID_BLOCK_ID:
            return "INVALID_BLOCK_ID";
        case AuditEvent::HASH_MISMATCH:
            return "HASH_MISMATCH";
        case AuditEvent::PERMISSION_DENIED:
            return "PERMISSION_DENIED";
        default:
            return "UNKNOWN";
    }
}

std::string AuditLogger::formatLogEntry(const AuditLogEntry& entry) {
    // Simple JSON formatting (avoiding dependency on nlohmann/json for now)
    std::ostringstream oss;
    oss << "{";
    oss << "\"timestamp\":\"" << entry.timestamp << "\",";
    oss << "\"event\":\"" << eventToString(entry.event) << "\",";
    oss << "\"details\":\"" << entry.details << "\"";

    if (!entry.metadata.empty()) {
        oss << ",\"metadata\":{";
        bool first = true;
        for (const auto& [key, value] : entry.metadata) {
            if (!first) oss << ",";
            oss << "\"" << key << "\":\"" << value << "\"";
            first = false;
        }
        oss << "}";
    }

    oss << "}";
    return oss.str();
}

void AuditLogger::writeLogEntry(const std::string& json) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Determine log file path
    std::string log_path = log_file_path_;
    if (log_path.empty()) {
        // Default to ~/.naab/logs/security.log
        const char* home = std::getenv("HOME");
        if (home) {
            log_path = std::string(home) + "/.naab/logs/security.log";
        } else {
            log_path = "/tmp/naab_security.log";
        }
    }

    // Create directory if it doesn't exist
    size_t last_slash = log_path.find_last_of('/');
    if (last_slash != std::string::npos) {
        std::string dir = log_path.substr(0, last_slash);
        mkdir(dir.c_str(), 0755);  // Create with appropriate permissions
    }

    // Check if rotation is needed
    checkRotation();

    // Open stream if not already open
    if (!log_stream_.is_open()) {
        log_stream_.open(log_path, std::ios::app);
    }

    // Write log entry
    if (log_stream_.is_open()) {
        log_stream_ << json << std::endl;
        log_stream_.flush();
    }
}

void AuditLogger::checkRotation() {
    if (log_file_path_.empty() || !log_stream_.is_open()) {
        return;
    }

    // Get current file size
    log_stream_.flush();
    struct stat st;
    if (stat(log_file_path_.c_str(), &st) == 0) {
        if (static_cast<size_t>(st.st_size) >= max_file_size_) {
            rotateLog();
        }
    }
}

void AuditLogger::rotateLog() {
    log_stream_.close();

    // Rotate: file.log -> file.log.1, file.log.1 -> file.log.2, etc.
    // Keep up to 5 rotated files
    for (int i = 4; i >= 1; i--) {
        std::string old_file = log_file_path_ + "." + std::to_string(i);
        std::string new_file = log_file_path_ + "." + std::to_string(i + 1);

        if (access(old_file.c_str(), F_OK) == 0) {
            rename(old_file.c_str(), new_file.c_str());
        }
    }

    // Move current log to .1
    std::string rotated = log_file_path_ + ".1";
    rename(log_file_path_.c_str(), rotated.c_str());

    // Stream will be reopened on next write
}

} // namespace security
} // namespace naab
