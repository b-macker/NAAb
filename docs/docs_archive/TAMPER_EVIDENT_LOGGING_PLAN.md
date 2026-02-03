# Phase 1 Item 8: Tamper-Evident Logging

**Estimated Time:** 5 days
**Priority:** HIGH (Security observability)
**Status:** In Progress

---

## Overview

Implement cryptographic integrity protection for audit logs to detect tampering, deletion, or modification of security events.

**Current State:**
- ✅ Basic audit logging (JSON format)
- ✅ Log rotation
- ❌ No tamper protection
- ❌ No integrity verification
- ❌ No append-only guarantees

**Goal State:**
- ✅ Hash-chained log entries
- ✅ Cryptographic signatures (optional)
- ✅ Tamper detection
- ✅ Integrity verification tool
- ✅ Append-only guarantees

---

## Design: Hash Chain Architecture

### Concept

Each log entry contains:
1. **Event data** (timestamp, event type, details)
2. **Previous entry hash** (links to prior entry)
3. **Current entry hash** (SHA-256 of: prev_hash + event_data)
4. **Sequence number** (monotonically increasing)

```
Entry 0 (Genesis):
  sequence: 0
  prev_hash: "0000000000..." (all zeros)
  data: {...}
  hash: SHA256(prev_hash + data)

Entry 1:
  sequence: 1
  prev_hash: <hash from Entry 0>
  data: {...}
  hash: SHA256(prev_hash + data)

Entry 2:
  sequence: 2
  prev_hash: <hash from Entry 1>
  data: {...}
  hash: SHA256(prev_hash + data)
```

**Tamper Detection:**
- Modifying Entry 1 changes its hash
- Entry 2's prev_hash won't match
- **Chain breaks → tampering detected**

---

## Implementation Plan

### Day 1: Core Hash Chain (4 hours)

**Files to Create:**
- `include/naab/tamper_evident_logger.h` - New API
- `src/runtime/tamper_evident_logger.cpp` - Implementation

**Features:**
1. SHA-256 hashing (using existing crypto_utils or OpenSSL)
2. Hash chain structure
3. Sequence numbering
4. Genesis block creation

**API Design:**
```cpp
namespace naab {
namespace security {

struct TamperEvidenceEntry {
    uint64_t sequence;
    std::string timestamp;
    std::string prev_hash;
    std::string event_type;
    std::string details;
    std::map<std::string, std::string> metadata;
    std::string hash;  // SHA-256(prev_hash + timestamp + event_type + details + metadata)
};

class TamperEvidenceLogger {
public:
    // Initialize logger with log file path
    explicit TamperEvidenceLogger(const std::string& log_path);
    
    // Log an event with automatic hash chain
    void logEvent(AuditEvent event, const std::string& details,
                  const std::map<std::string, std::string>& metadata = {});
    
    // Verify integrity of entire log chain
    struct VerificationResult {
        bool is_valid;
        std::vector<std::string> errors;  // Empty if valid
        uint64_t total_entries;
        uint64_t verified_entries;
    };
    VerificationResult verifyIntegrity() const;
    
    // Get last entry's hash (for chain continuation)
    std::string getLastHash() const;
    
    // Get current sequence number
    uint64_t getSequence() const;
    
private:
    std::string computeHash(const TamperEvidenceEntry& entry) const;
    void writeEntry(const TamperEvidenceEntry& entry);
    std::string log_file_path_;
    std::string last_hash_;
    uint64_t sequence_;
    std::mutex mutex_;
};

} // namespace security
} // namespace naab
```

---

### Day 2: Integration with Existing AuditLogger (3 hours)

**Modify:**
- `src/runtime/audit_logger.cpp` - Use TamperEvidenceLogger internally

**Strategy:**
- Keep existing `AuditLogger` API unchanged (backward compatible)
- Add new mode: `setTamperEvidence(bool enabled)`
- When enabled, wrap events with hash chain

**Example:**
```cpp
// Old code (still works):
AuditLogger::log(AuditEvent::BLOCK_LOAD, "Block loaded");

// Internally (if tamper evidence enabled):
tamper_logger_->logEvent(event, details, metadata);
```

---

### Day 3: Verification Tool (4 hours)

**Create:**
- `src/cli/verify_audit.cpp` - Standalone verification tool
- Add to CMakeLists.txt as `naab-verify-audit`

**Features:**
1. Read tamper-evident log file
2. Verify hash chain integrity
3. Detect missing entries (sequence gaps)
4. Detect modified entries (hash mismatch)
5. Report detailed findings

**CLI Usage:**
```bash
naab-verify-audit ~/.naab/logs/security.log

# Output:
Verifying audit log: security.log
✓ Entry 0: Valid (genesis block)
✓ Entry 1: Valid (prev_hash matches)
✓ Entry 2: Valid (prev_hash matches)
...
✗ Entry 47: TAMPERED! (hash mismatch)
  Expected hash: abc123...
  Actual hash:   def456...

Summary:
  Total entries: 100
  Valid entries: 99
  Tampered: 1
  Missing: 0
  Status: ✗ TAMPERED LOG DETECTED
```

---

### Day 4: Optional Signatures (4 hours)

**Enhance:**
- Add HMAC signatures (using shared secret key)
- Add Ed25519 signatures (using keypair)

**New API:**
```cpp
class TamperEvidenceLogger {
public:
    // Enable HMAC signing (symmetric key)
    void enableHMAC(const std::string& secret_key);
    
    // Enable Ed25519 signing (asymmetric keypair)
    void enableSignature(const std::string& private_key_path);
    
    // Verification now also checks signatures
    VerificationResult verifyIntegrity(
        const std::string& public_key_path = ""  // For Ed25519
    ) const;
};
```

**Benefits:**
- HMAC: Fast, simple, requires shared secret
- Ed25519: Public verifiability, no shared secret needed

---

### Day 5: Testing & Documentation (4 hours)

**Tests:**
1. Unit tests for hash computation
2. Integration tests for log chain
3. Tamper detection tests (modify entries and verify detection)
4. Signature verification tests
5. Multi-rotation tests (ensure chain continues across log rotations)

**Test File:** `tests/unit/tamper_evident_logger_test.cpp`

**Documentation:**
1. `docs/TAMPER_EVIDENT_LOGGING.md` - User guide
2. `docs/SECURITY_LOGGING_BEST_PRACTICES.md` - Recommendations
3. Update `docs/SAFETY_AUDIT.md` - Mark item as complete

---

## Security Properties

### What This Protects Against

✅ **Log Modification:**
- Changing event details breaks hash chain
- Verification detects tampering

✅ **Log Deletion:**
- Missing sequence numbers detected
- Gap in sequence = entries deleted

✅ **Log Reordering:**
- Sequence numbers enforce order
- Hash chain prevents reordering

✅ **Backdating:**
- Timestamps included in hash
- Cannot change timestamp without breaking chain

### What This Does NOT Protect Against

❌ **Complete Log File Deletion:**
- If entire file deleted, no evidence remains
- Mitigation: Remote log forwarding (future work)

❌ **Attacker with Key Access:**
- If attacker has signing key, can create valid fake entries
- Mitigation: HSM/TPM key storage (future work)

❌ **Real-time Log Manipulation:**
- Attacker could modify logs before they're written
- Mitigation: Kernel-level audit logging (future work)

---

## Example Log Format

### Genesis Block (First Entry)
```json
{
  "sequence": 0,
  "timestamp": "2026-01-31T12:00:00.000Z",
  "prev_hash": "0000000000000000000000000000000000000000000000000000000000000000",
  "event": "LOG_INIT",
  "details": "Tamper-evident logging initialized",
  "metadata": {
    "version": "1.0",
    "hostname": "dev-server"
  },
  "hash": "a1b2c3d4e5f6...",
  "signature": "ed25519:abc123..." (optional)
}
```

### Regular Entry
```json
{
  "sequence": 42,
  "timestamp": "2026-01-31T12:05:30.123Z",
  "prev_hash": "a1b2c3d4e5f6...",
  "event": "BLOCK_LOAD",
  "details": "Block loaded successfully",
  "metadata": {
    "block_id": "BLOCK-JS-STRING",
    "hash": "sha256:xyz789..."
  },
  "hash": "b2c3d4e5f6a1...",
  "signature": "ed25519:def456..." (optional)
}
```

---

## Performance Considerations

### Hash Computation Cost
- SHA-256 is fast (~300 MB/s on modern CPUs)
- Per-event overhead: < 1ms
- Acceptable for audit logging (not high-frequency)

### Storage Overhead
- Hash: 64 hex chars (32 bytes)
- Signature: ~128 bytes (Ed25519)
- Per-entry overhead: ~200 bytes
- For 1M events/day: +200 MB storage

### Verification Cost
- Linear scan of log file: O(n)
- 10,000 entries: ~100ms
- 1,000,000 entries: ~10 seconds
- Acceptable for offline verification

---

## Success Criteria

- [ ] Hash chain implementation complete
- [ ] Integration with AuditLogger complete
- [ ] Verification tool working
- [ ] Can detect modified entries
- [ ] Can detect deleted entries
- [ ] Can detect reordered entries
- [ ] Unit tests pass (>90% coverage)
- [ ] Documentation complete
- [ ] Safety audit updated

---

## Deliverables

1. **Code:**
   - `include/naab/tamper_evident_logger.h`
   - `src/runtime/tamper_evident_logger.cpp`
   - `src/cli/verify_audit.cpp`
   - Updated `src/runtime/audit_logger.cpp`

2. **Tests:**
   - `tests/unit/tamper_evident_logger_test.cpp`
   - 20+ test cases

3. **Documentation:**
   - `docs/TAMPER_EVIDENT_LOGGING.md`
   - `docs/SECURITY_LOGGING_BEST_PRACTICES.md`
   - Updated `docs/SAFETY_AUDIT.md`

4. **Tools:**
   - `naab-verify-audit` CLI tool

---

**Next Steps:** Begin Day 1 implementation (Core Hash Chain)
