#pragma once

#include <string>
#include <vector>
#include <utility>

namespace naab {
namespace security {

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
    // Skips corrupt/unreadable files with stderr warning.
    static std::vector<std::pair<std::string, std::string>> loadKeys();

    // Install a public key PEM into the trust store.
    // Writes <fingerprint>.pub. Creates default.pub copy if first key.
    static bool installKey(const std::string& public_key_pem);

    // List installed key fingerprints.
    static std::vector<std::string> listKeyFingerprints();
};

} // namespace security
} // namespace naab
