#include "naab/tamper_evident_logger.h"
#include "naab/audit_logger.h"
#include "naab/crypto_utils.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <fmt/core.h>

namespace naab {
namespace security {

// Genesis block hash (all zeros)
static const std::string GENESIS_HASH = std::string(64, '0');

//=============================================================================
// TamperEvidenceEntry Implementation
//=============================================================================

std::string TamperEvidenceEntry::toCanonicalString() const {
    // Deterministic ordering for hash computation
    std::ostringstream oss;
    oss << sequence << "|"
        << timestamp << "|"
        << prev_hash << "|"
        << event_type << "|"
        << details;

    // Add metadata in sorted order (for determinism)
    if (!metadata.empty()) {
        oss << "|metadata:";
        std::vector<std::string> keys;
        for (const auto& [key, _] : metadata) {
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());

        for (const auto& key : keys) {
            oss << key << "=" << metadata.at(key) << ";";
        }
    }

    return oss.str();
}

// Helper functions for JSON escaping/unescaping/parsing
namespace {
    // Find closing quote in JSON string, skipping escaped quotes
    size_t findClosingQuote(const std::string& json, size_t start) {
        size_t end = start;
        while (end < json.length()) {
            end = json.find("\"", end);
            if (end == std::string::npos) return std::string::npos;

            // Count preceding backslashes
            size_t backslashes = 0;
            size_t check = end;
            while (check > start && json[check - 1] == '\\') {
                backslashes++;
                check--;
            }

            // If even number of backslashes, the quote is not escaped
            if (backslashes % 2 == 0) {
                return end;  // Found the closing quote
            }

            // This quote was escaped, continue searching
            end++;
        }
        return std::string::npos;
    }
    std::string escapeJSON(const std::string& str) {
        std::ostringstream oss;
        for (char c : str) {
            switch (c) {
                case '"':  oss << "\\\""; break;
                case '\\': oss << "\\\\"; break;
                case '\b': oss << "\\b"; break;
                case '\f': oss << "\\f"; break;
                case '\n': oss << "\\n"; break;
                case '\r': oss << "\\r"; break;
                case '\t': oss << "\\t"; break;
                default:
                    if (c < 32) {
                        // Control characters - escape as \uXXXX
                        oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)(unsigned char)c;
                    } else {
                        oss << c;
                    }
            }
        }
        return oss.str();
    }

    std::string unescapeJSON(const std::string& str) {
        std::ostringstream oss;
        for (size_t i = 0; i < str.length(); ++i) {
            if (str[i] == '\\' && i + 1 < str.length()) {
                switch (str[i + 1]) {
                    case '"':  oss << '"'; i++; break;
                    case '\\': oss << '\\'; i++; break;
                    case 'b':  oss << '\b'; i++; break;
                    case 'f':  oss << '\f'; i++; break;
                    case 'n':  oss << '\n'; i++; break;
                    case 'r':  oss << '\r'; i++; break;
                    case 't':  oss << '\t'; i++; break;
                    case 'u':  // Unicode escape \uXXXX
                        if (i + 5 < str.length()) {
                            std::string hex = str.substr(i + 2, 4);
                            int code = std::stoi(hex, nullptr, 16);
                            oss << (char)code;
                            i += 5;
                        }
                        break;
                    default:
                        oss << str[i];  // Unknown escape, keep as-is
                }
            } else {
                oss << str[i];
            }
        }
        return oss.str();
    }
}

std::string TamperEvidenceEntry::toJSON() const {
    std::ostringstream oss;
    oss << "{";
    oss << "\"sequence\":" << sequence << ",";
    oss << "\"timestamp\":\"" << escapeJSON(timestamp) << "\",";
    oss << "\"prev_hash\":\"" << prev_hash << "\",";
    oss << "\"event\":\"" << escapeJSON(event_type) << "\",";
    oss << "\"details\":\"" << escapeJSON(details) << "\"";

    if (!metadata.empty()) {
        oss << ",\"metadata\":{";
        bool first = true;
        for (const auto& [key, value] : metadata) {
            if (!first) oss << ",";
            oss << "\"" << escapeJSON(key) << "\":\"" << escapeJSON(value) << "\"";
            first = false;
        }
        oss << "}";
    }

    oss << ",\"hash\":\"" << hash << "\"";
    oss << ",\"signature\":\"" << signature << "\"";  // Always include signature field

    oss << "}";
    return oss.str();
}

TamperEvidenceEntry TamperEvidenceEntry::fromJSON(const std::string& json) {
    TamperEvidenceEntry entry;

    // Simple JSON parsing (enough for our structured format)
    // Production code would use nlohmann/json, but keeping dependencies minimal

    size_t pos = 0;
    auto findNext = [&](const std::string& pattern) -> std::string {
        size_t start = json.find(pattern, pos);
        if (start == std::string::npos) return "";

        start += pattern.length();
        size_t end = findClosingQuote(json, start);
        if (end == std::string::npos) return "";

        pos = end + 1;
        return unescapeJSON(json.substr(start, end - start));
    };

    auto findNextInt = [&](const std::string& pattern) -> uint64_t {
        size_t start = json.find(pattern, pos);
        if (start == std::string::npos) return 0;

        start += pattern.length();
        size_t end = json.find_first_of(",}", start);
        if (end == std::string::npos) return 0;

        pos = end;
        std::string num_str = json.substr(start, end - start);
        return std::stoull(num_str);
    };

    pos = 0;
    entry.sequence = findNextInt("\"sequence\":");
    entry.timestamp = findNext("\"timestamp\":\"");
    entry.prev_hash = findNext("\"prev_hash\":\"");
    entry.event_type = findNext("\"event\":\"");
    entry.details = findNext("\"details\":\"");
    entry.hash = findNext("\"hash\":\"");

    // Check for signature (optional)
    size_t sig_pos = json.find("\"signature\":\"");
    if (sig_pos != std::string::npos) {
        sig_pos += 14; // length of "\"signature\":\""
        size_t sig_end = findClosingQuote(json, sig_pos);
        if (sig_end != std::string::npos) {
            entry.signature = unescapeJSON(json.substr(sig_pos, sig_end - sig_pos));
        }
    }

    // Parse metadata (simplified - assumes no nested objects)
    size_t meta_start = json.find("\"metadata\":{");
    if (meta_start != std::string::npos) {
        size_t meta_end = json.find("}", meta_start);
        if (meta_end != std::string::npos) {
            std::string meta_str = json.substr(meta_start + 12, meta_end - (meta_start + 12));
            // Parse key-value pairs
            size_t m_pos = 0;
            while (m_pos < meta_str.length()) {
                size_t key_start = meta_str.find("\"", m_pos);
                if (key_start == std::string::npos) break;
                key_start++;
                size_t key_end = findClosingQuote(meta_str, key_start);
                if (key_end == std::string::npos) break;

                std::string key = unescapeJSON(meta_str.substr(key_start, key_end - key_start));

                size_t val_start = meta_str.find("\"", key_end + 1);
                if (val_start == std::string::npos) break;
                val_start++;
                size_t val_end = findClosingQuote(meta_str, val_start);
                if (val_end == std::string::npos) break;

                std::string value = unescapeJSON(meta_str.substr(val_start, val_end - val_start));

                entry.metadata[key] = value;
                m_pos = val_end + 1;
            }
        }
    }

    return entry;
}

//=============================================================================
// VerificationResult Implementation
//=============================================================================

std::string VerificationResult::getReport() const {
    std::ostringstream oss;

    oss << "=== Tamper-Evident Log Verification Report ===\n\n";

    oss << "Total Entries: " << total_entries << "\n";
    oss << "Verified Entries: " << verified_entries << "\n";
    oss << "Status: " << (is_valid ? "✓ VALID" : "✗ TAMPERED") << "\n\n";

    if (!tampered_sequences.empty()) {
        oss << "Tampered Entries (" << tampered_sequences.size() << "):\n";
        for (auto seq : tampered_sequences) {
            oss << "  - Sequence " << seq << "\n";
        }
        oss << "\n";
    }

    if (!missing_sequences.empty()) {
        oss << "Missing Entries (" << missing_sequences.size() << "):\n";
        for (auto seq : missing_sequences) {
            oss << "  - Sequence " << seq << "\n";
        }
        oss << "\n";
    }

    if (!errors.empty()) {
        oss << "Errors:\n";
        for (const auto& error : errors) {
            oss << "  - " << error << "\n";
        }
    }

    return oss.str();
}

//=============================================================================
// TamperEvidenceLogger Implementation
//=============================================================================

TamperEvidenceLogger::TamperEvidenceLogger(const std::string& log_path)
    : log_file_path_(log_path)
    , last_hash_(GENESIS_HASH)
    , sequence_(0)
    , hmac_enabled_(false)
{
    // Create directory if it doesn't exist
    std::filesystem::path log_dir = std::filesystem::path(log_path).parent_path();
    if (!log_dir.empty() && !std::filesystem::exists(log_dir)) {
        std::filesystem::create_directories(log_dir);
    }

    // Load last entry if file exists
    if (std::filesystem::exists(log_path)) {
        loadLastEntry();
    } else {
        // Create new log with genesis block
        initializeLog(log_path);
        loadLastEntry();
    }
}

TamperEvidenceLogger::~TamperEvidenceLogger() {
    flush();
}

void TamperEvidenceLogger::logEvent(AuditEvent event, const std::string& details,
                                     const std::map<std::string, std::string>& metadata) {
    std::lock_guard<std::mutex> lock(mutex_);

    TamperEvidenceEntry entry;
    entry.sequence = ++sequence_;
    entry.timestamp = getCurrentTimestamp();
    entry.prev_hash = last_hash_;
    entry.event_type = eventToString(event);
    entry.details = details;
    entry.metadata = metadata;

    // Compute hash
    entry.hash = computeHash(entry);

    // Compute signature if HMAC enabled
    if (hmac_enabled_) {
        entry.signature = "hmac-sha256:" + computeHMAC(entry.hash, hmac_key_);
    }

    // Write to log
    writeEntry(entry);

    // Update last hash for next entry
    last_hash_ = entry.hash;
}

std::string TamperEvidenceLogger::computeHash(const TamperEvidenceEntry& entry) const {
    std::string canonical = entry.toCanonicalString();
    return CryptoUtils::sha256(canonical);
}

std::string TamperEvidenceLogger::computeHMAC(const std::string& data, const std::string& key) const {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;

    HMAC(EVP_sha256(),
         key.c_str(), static_cast<int>(key.length()),
         reinterpret_cast<const unsigned char*>(data.c_str()), static_cast<int>(data.length()),
         result, &result_len);

    // Convert to hex
    std::ostringstream oss;
    for (unsigned int i = 0; i < result_len; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)result[i];
    }

    return oss.str();
}

void TamperEvidenceLogger::writeEntry(const TamperEvidenceEntry& entry) {
    // Open in append mode
    if (!log_stream_.is_open()) {
        log_stream_.open(log_file_path_, std::ios::app);
    }

    if (!log_stream_.is_open()) {
        throw std::runtime_error("Failed to open tamper-evident log: " + log_file_path_);
    }

    // Write JSON entry
    log_stream_ << entry.toJSON() << "\n";
    log_stream_.flush();
}

void TamperEvidenceLogger::loadLastEntry() {
    std::ifstream file(log_file_path_);
    if (!file.is_open()) {
        return;  // No existing log
    }

    std::string last_line;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            last_line = line;
        }
    }

    if (!last_line.empty()) {
        TamperEvidenceEntry last_entry = TamperEvidenceEntry::fromJSON(last_line);
        last_hash_ = last_entry.hash;
        sequence_ = last_entry.sequence;
    }
}

TamperEvidenceEntry TamperEvidenceLogger::createGenesisBlock() const {
    TamperEvidenceEntry genesis;
    genesis.sequence = 0;
    genesis.timestamp = getCurrentTimestamp();
    genesis.prev_hash = GENESIS_HASH;
    genesis.event_type = "LOG_INIT";
    genesis.details = "Tamper-evident logging initialized";
    genesis.metadata["version"] = "1.0";

    // Compute hash
    std::string canonical = genesis.toCanonicalString();
    genesis.hash = CryptoUtils::sha256(canonical);

    return genesis;
}

void TamperEvidenceLogger::initializeLog(const std::string& log_path) {
    // Create directory if needed
    std::filesystem::path log_dir = std::filesystem::path(log_path).parent_path();
    if (!log_dir.empty() && !std::filesystem::exists(log_dir)) {
        std::filesystem::create_directories(log_dir);
    }

    // Create temporary logger to write genesis block
    std::ofstream file(log_path, std::ios::trunc);  // Truncate existing file
    if (!file.is_open()) {
        throw std::runtime_error("Failed to create tamper-evident log: " + log_path);
    }

    TamperEvidenceLogger temp_logger(log_path);
    TamperEvidenceEntry genesis = temp_logger.createGenesisBlock();

    file << genesis.toJSON() << "\n";
    file.flush();
}

VerificationResult TamperEvidenceLogger::verifyIntegrity() const {
    return verifyIntegrity("");  // No HMAC verification
}

VerificationResult TamperEvidenceLogger::verifyIntegrity(const std::string& hmac_key) const {
    VerificationResult result;
    result.is_valid = true;
    result.total_entries = 0;
    result.verified_entries = 0;

    std::ifstream file(log_file_path_);
    if (!file.is_open()) {
        result.is_valid = false;
        result.errors.push_back("Cannot open log file: " + log_file_path_);
        return result;
    }

    std::string line;
    std::string expected_prev_hash = GENESIS_HASH;
    uint64_t expected_sequence = 0;

    while (std::getline(file, line)) {
        if (line.empty()) continue;

        result.total_entries++;

        try {
            TamperEvidenceEntry entry = TamperEvidenceEntry::fromJSON(line);

            // Verify sequence number
            if (entry.sequence != expected_sequence) {
                result.is_valid = false;
                result.errors.push_back(fmt::format(
                    "Sequence mismatch at entry {}: expected {}, got {}",
                    result.total_entries, expected_sequence, entry.sequence));

                // Check for missing sequences
                for (uint64_t seq = expected_sequence; seq < entry.sequence; seq++) {
                    result.missing_sequences.push_back(seq);
                }
            }

            // Verify previous hash
            if (entry.prev_hash != expected_prev_hash) {
                result.is_valid = false;
                result.tampered_sequences.push_back(entry.sequence);
                result.errors.push_back(fmt::format(
                    "Hash chain broken at sequence {}: expected prev_hash {}, got {}",
                    entry.sequence, expected_prev_hash.substr(0, 16) + "...",
                    entry.prev_hash.substr(0, 16) + "..."));
            }

            // Verify current hash
            std::string computed_hash = CryptoUtils::sha256(entry.toCanonicalString());
            if (entry.hash != computed_hash) {
                result.is_valid = false;
                result.tampered_sequences.push_back(entry.sequence);
                result.errors.push_back(fmt::format(
                    "Hash mismatch at sequence {}: computed {}, stored {}",
                    entry.sequence, computed_hash.substr(0, 16) + "...",
                    entry.hash.substr(0, 16) + "..."));
            } else {
                result.verified_entries++;
            }

            // Verify HMAC if provided
            if (!hmac_key.empty() && !entry.signature.empty()) {
                // Extract HMAC from signature (format: "hmac-sha256:HEXVALUE")
                size_t colon_pos = entry.signature.find(':');
                if (colon_pos != std::string::npos) {
                    std::string stored_hmac = entry.signature.substr(colon_pos + 1);
                    std::string computed_hmac = computeHMAC(entry.hash, hmac_key);

                    if (stored_hmac != computed_hmac) {
                        result.is_valid = false;
                        result.tampered_sequences.push_back(entry.sequence);
                        result.errors.push_back(fmt::format(
                            "HMAC verification failed at sequence {}", entry.sequence));
                    }
                }
            }

            // Update expectations for next entry
            expected_prev_hash = entry.hash;
            expected_sequence = entry.sequence + 1;

        } catch (const std::exception& e) {
            result.is_valid = false;
            result.errors.push_back(fmt::format(
                "Failed to parse entry {}: {}", result.total_entries, e.what()));
        }
    }

    return result;
}

std::string TamperEvidenceLogger::getLastHash() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_hash_;
}

uint64_t TamperEvidenceLogger::getSequence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sequence_;
}

void TamperEvidenceLogger::enableHMAC(const std::string& secret_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    hmac_enabled_ = true;
    hmac_key_ = secret_key;
}

void TamperEvidenceLogger::disableHMAC() {
    std::lock_guard<std::mutex> lock(mutex_);
    hmac_enabled_ = false;
    hmac_key_.clear();
}

void TamperEvidenceLogger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_stream_.is_open()) {
        log_stream_.flush();
    }
}

std::string TamperEvidenceLogger::eventToString(AuditEvent event) const {
    // Forward to AuditLogger's implementation
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

std::string TamperEvidenceLogger::getCurrentTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';

    return oss.str();
}

} // namespace security
} // namespace naab
