#pragma once

#include <string>

namespace naab {
namespace security {

// Cryptographic utilities for code integrity verification
class CryptoUtils {
public:
    // Compute SHA256 hash of data and return as hex string
    static std::string sha256(const std::string& data);

    // Compute SHA256 hash of file contents and return as hex string
    static std::string sha256File(const std::string& filepath);

    // Verify that data matches expected SHA256 hash (hex string)
    // Uses constant-time comparison to prevent timing attacks
    static bool verifyHash(const std::string& data, const std::string& expected_hash);

    // Convert binary data to hexadecimal string
    static std::string toHex(const unsigned char* data, size_t length);

    // Convert hexadecimal string to binary data
    static std::string fromHex(const std::string& hex);

    // Compute HMAC-SHA256 of data with key; returns hex string.
    // Only available when OpenSSL is present (HAVE_OPENSSL defined).
    static std::string hmacSha256(const std::string& data, const std::string& key);

    // Constant-time string comparison (prevents timing attacks on HMAC/hash comparisons).
    static bool constantTimeCompare(const std::string& a, const std::string& b);

    // --- Ed25519 asymmetric signing (V-SC-009) ---

    // Generate Ed25519 keypair. Writes PEM-encoded strings.
    static bool ed25519Keygen(std::string& out_private_pem, std::string& out_public_pem);

    // Sign data with Ed25519 private key (PEM). Returns base64-encoded 64-byte signature.
    static std::string ed25519Sign(const std::string& data, const std::string& private_key_pem);

    // Verify Ed25519 signature (base64) against data using public key (PEM).
    static bool ed25519Verify(const std::string& data, const std::string& signature_b64,
                              const std::string& public_key_pem);

    // Derive public key PEM from Ed25519 private key PEM.
    static std::string ed25519PublicFromPrivate(const std::string& private_pem);

    // SHA-256 fingerprint of Ed25519 public key (last 16 hex chars).
    static std::string ed25519Fingerprint(const std::string& pem);

    // Base64 encode/decode (OpenSSL BIO-based, no newlines).
    static std::string toBase64(const unsigned char* data, size_t length);
    static std::string fromBase64(const std::string& b64);

private:
};

} // namespace security
} // namespace naab
