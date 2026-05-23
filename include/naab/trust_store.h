#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace naab {
namespace security {

// Key metadata — sidecar for Ed25519 public keys (.meta.json)
struct KeyMetadata {
    std::string fingerprint;
    std::string label;
    int64_t created_at = 0;     // unix epoch
    int64_t expires_at = 0;     // 0 = no expiry
    bool revoked = false;
    std::string revoked_reason;
    int64_t revoked_at = 0;
};

// V-SC-009: External trust store for Ed25519 public keys.
// Located at ~/.naab/trusted-keys/ — outside any project sandbox.
// When keys exist in the trust store, all governance files must be signed.
class TrustStore {
public:
    // Get trust store directory path (~/.naab/trusted-keys/)
    static std::string getStorePath();

    // Check if trust store exists and contains at least one .pub key
    static bool hasKeys();

    // Load all public keys from trust store.
    // Returns vector of {fingerprint, pem_content} pairs.
    // Skips corrupt/unreadable/expired/revoked files with stderr warning.
    static std::vector<std::pair<std::string, std::string>> loadKeys();

    // Install a public key PEM into the trust store.
    // Writes <fingerprint>.pub and stamps created_at in .meta.json.
    // Creates default.pub copy if first key.
    static bool installKey(const std::string& public_key_pem);

    // List installed key fingerprints.
    static std::vector<std::string> listKeyFingerprints();

    // --- Key Metadata (Authority Decay) ---

    // Load metadata sidecar for a key. Returns default metadata if file missing.
    static KeyMetadata loadKeyMetadata(const std::string& fingerprint);

    // Save metadata sidecar for a key.
    static bool saveKeyMetadata(const std::string& fingerprint, const KeyMetadata& meta);

    // Load all keys with their metadata.
    static std::vector<KeyMetadata> loadKeysWithMetadata();

    // Check if a key is expired (expires_at > 0 && now > expires_at).
    static bool isKeyExpired(const KeyMetadata& meta);

    // Check if a key is revoked (from .meta.json).
    static bool isKeyRevoked(const std::string& fingerprint);

    // Revoke a key: sets revoked=true, revoked_at=now in .meta.json.
    static bool revokeKey(const std::string& fingerprint, const std::string& reason);
};

} // namespace security
} // namespace naab
