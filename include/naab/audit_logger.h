#pragma once

#include <string>
#include <map>
#include <mutex>
#include <fstream>
#include <memory>

// Forward declarations
namespace naab { namespace security { class TamperEvidenceLogger; } }

namespace naab {
namespace security {

// Security audit event types
enum class AuditEvent {
    BLOCK_LOAD,           // Block loaded successfully
    BLOCK_EXECUTE,        // Block executed
    SECURITY_VIOLATION,   // Security policy violation
    TIMEOUT,              // Execution timeout
    INVALID_PATH,         // Path traversal attempt
    INVALID_BLOCK_ID,     // Invalid block ID format
    HASH_MISMATCH,        // Code integrity check failed
    PERMISSION_DENIED     // Permission/access denied
};

// Audit log entry
struct AuditLogEntry {
    std::string timestamp;     // ISO 8601 timestamp
    AuditEvent event;          // Event type
    std::string details;       // Event details
    std::map<std::string, std::string> metadata;  // Additional metadata
};

// Audit logger for security events
class AuditLogger {
public:
    // Log a security event with details
    static void log(AuditEvent event, const std::string& details);

    // Log with additional metadata
    static void logWithMetadata(AuditEvent event, const std::string& details,
                                 const std::map<std::string, std::string>& metadata);

    // Convenience methods for specific events
    static void logBlockLoad(const std::string& block_id, const std::string& hash);
    static void logBlockExecute(const std::string& block_id, const std::string& language);
    static void logSecurityViolation(const std::string& reason);
    static void logTimeout(const std::string& operation, unsigned int timeout_seconds);
    static void logInvalidPath(const std::string& path, const std::string& reason);
    static void logHashMismatch(const std::string& block_id, const std::string& expected,
                                 const std::string& actual);

    // Configure logger
    static void setLogFile(const std::string& filepath);
    static void setMaxFileSize(size_t max_size_bytes);
    static void setEnabled(bool enabled);

    // Phase 1 Item 8: Tamper-evident logging
    static void setTamperEvidence(bool enabled);
    static void enableHMAC(const std::string& secret_key);
    static void disableHMAC();
    static bool isTamperEvidenceEnabled();

    // Flush logs to disk
    static void flush();

private:
    // Get current timestamp in ISO 8601 format
    static std::string getCurrentTimestamp();

    // Convert AuditEvent to string
    static std::string eventToString(AuditEvent event);

    // Format log entry as JSON
    static std::string formatLogEntry(const AuditLogEntry& entry);

    // Write log entry to file
    static void writeLogEntry(const std::string& json);

    // Check if log rotation is needed and rotate if necessary
    static void checkRotation();

    // Rotate log file
    static void rotateLog();

    // Static members
    static std::string log_file_path_;
    static size_t max_file_size_;
    static bool enabled_;
    static std::mutex mutex_;
    static std::ofstream log_stream_;

    // Phase 1 Item 8: Tamper-evident logging
    static bool tamper_evidence_enabled_;
    static std::unique_ptr<TamperEvidenceLogger> tamper_logger_;
};

} // namespace security
} // namespace naab
