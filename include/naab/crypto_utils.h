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

private:
    // Constant-time string comparison
    static bool constantTimeCompare(const std::string& a, const std::string& b);
};

} // namespace security
} // namespace naab
