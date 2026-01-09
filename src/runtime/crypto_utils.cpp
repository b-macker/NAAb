#include "naab/crypto_utils.h"
#include <openssl/sha.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <cstring>

namespace naab {
namespace security {

std::string CryptoUtils::sha256(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];

    SHA256(reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), hash);

    return toHex(hash, SHA256_DIGEST_LENGTH);
}

std::string CryptoUtils::sha256File(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file for hashing: " + filepath);
    }

    // Read file in chunks
    constexpr size_t BUFFER_SIZE = 8192;
    char buffer[BUFFER_SIZE];

    SHA256_CTX sha256_ctx;
    SHA256_Init(&sha256_ctx);

    while (file.read(buffer, BUFFER_SIZE) || file.gcount() > 0) {
        SHA256_Update(&sha256_ctx, buffer, file.gcount());
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha256_ctx);

    return toHex(hash, SHA256_DIGEST_LENGTH);
}

bool CryptoUtils::verifyHash(const std::string& data, const std::string& expected_hash) {
    if (expected_hash.empty()) {
        return false;
    }

    std::string actual_hash = sha256(data);

    // Use constant-time comparison to prevent timing attacks
    return constantTimeCompare(actual_hash, expected_hash);
}

std::string CryptoUtils::toHex(const unsigned char* data, size_t length) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    for (size_t i = 0; i < length; i++) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }

    return oss.str();
}

std::string CryptoUtils::fromHex(const std::string& hex) {
    if (hex.length() % 2 != 0) {
        throw std::invalid_argument("Hex string must have even length");
    }

    std::string result;
    result.reserve(hex.length() / 2);

    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        char byte = static_cast<char>(std::stoi(byte_str, nullptr, 16));
        result.push_back(byte);
    }

    return result;
}

bool CryptoUtils::constantTimeCompare(const std::string& a, const std::string& b) {
    // Constant-time comparison to prevent timing attacks
    // Always compare all bytes even if early mismatch is found

    if (a.length() != b.length()) {
        return false;
    }

    volatile unsigned char result = 0;

    for (size_t i = 0; i < a.length(); i++) {
        result |= static_cast<unsigned char>(a[i] ^ b[i]);
    }

    return result == 0;
}

} // namespace security
} // namespace naab
