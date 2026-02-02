#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <cstdint>
#include <fstream>

namespace naab {
namespace security {

// Forward declaration
enum class AuditEvent;

// Tamper-evident log entry with hash chain
struct TamperEvidenceEntry {
    uint64_t sequence;                               // Monotonically increasing sequence number
    std::string timestamp;                           // ISO 8601 timestamp
    std::string prev_hash;                           // Hash of previous entry (links chain)
    std::string event_type;                          // Event type (e.g., "BLOCK_LOAD")
    std::string details;                             // Event details
    std::map<std::string, std::string> metadata;     // Additional metadata
    std::string hash;                                // SHA-256 of this entry
    std::string signature;                           // Optional HMAC/Ed25519 signature

    // Compute canonical string for hashing (deterministic ordering)
    std::string toCanonicalString() const;

    // Serialize to JSON
    std::string toJSON() const;

    // Deserialize from JSON
    static TamperEvidenceEntry fromJSON(const std::string& json);
};

// Verification result for integrity checking
struct VerificationResult {
    bool is_valid;                                   // Overall validity
    std::vector<std::string> errors;                 // Error messages (empty if valid)
    std::vector<uint64_t> tampered_sequences;        // Sequence numbers of tampered entries
    std::vector<uint64_t> missing_sequences;         // Missing sequence numbers
    uint64_t total_entries;                          // Total entries in log
    uint64_t verified_entries;                       // Successfully verified entries

    // Generate human-readable report
    std::string getReport() const;
};

// Tamper-evident logger with cryptographic hash chains
class TamperEvidenceLogger {
public:
    // Initialize logger with log file path
    explicit TamperEvidenceLogger(const std::string& log_path);

    // Destructor - flush any pending writes
    ~TamperEvidenceLogger();

    // Log an event with automatic hash chain
    void logEvent(AuditEvent event, const std::string& details,
                  const std::map<std::string, std::string>& metadata = {});

    // Verify integrity of entire log chain
    VerificationResult verifyIntegrity() const;

    // Verify integrity with HMAC key
    VerificationResult verifyIntegrity(const std::string& hmac_key) const;

    // Get last entry's hash (for chain continuation)
    std::string getLastHash() const;

    // Get current sequence number
    uint64_t getSequence() const;

    // Enable HMAC signing with secret key
    void enableHMAC(const std::string& secret_key);

    // Disable HMAC signing
    void disableHMAC();

    // Flush log to disk
    void flush();

    // Initialize new log file with genesis block
    static void initializeLog(const std::string& log_path);

private:
    // Compute SHA-256 hash of entry
    std::string computeHash(const TamperEvidenceEntry& entry) const;

    // Compute HMAC-SHA256 signature
    std::string computeHMAC(const std::string& data, const std::string& key) const;

    // Write entry to log file (append-only)
    void writeEntry(const TamperEvidenceEntry& entry);

    // Load last entry from log file (for chain continuation)
    void loadLastEntry();

    // Create genesis block (first entry in chain)
    TamperEvidenceEntry createGenesisBlock() const;

    // Convert AuditEvent enum to string
    std::string eventToString(AuditEvent event) const;

    // Get current timestamp in ISO 8601 format
    std::string getCurrentTimestamp() const;

    // Member variables
    std::string log_file_path_;                      // Path to log file
    std::string last_hash_;                          // Hash of last entry
    uint64_t sequence_;                              // Current sequence number
    mutable std::mutex mutex_;                       // Thread safety (mutable for const methods)
    std::ofstream log_stream_;                       // Output stream
    bool hmac_enabled_;                              // HMAC signing enabled
    std::string hmac_key_;                           // HMAC secret key
};

} // namespace security
} // namespace naab
