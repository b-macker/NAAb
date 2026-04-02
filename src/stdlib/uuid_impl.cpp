//
// NAAb Standard Library - UUID Module
// UUID v4 (random) and v5 (SHA-1 deterministic), validation
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#ifdef HAS_OPENSSL
#include <openssl/sha.h>
#endif

namespace naab {
namespace stdlib {

// Format 16 bytes as a UUID string: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
static std::string formatUUID(const uint8_t bytes[16]) {
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0],  bytes[1],  bytes[2],  bytes[3],
        bytes[4],  bytes[5],
        bytes[6],  bytes[7],
        bytes[8],  bytes[9],
        bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string(buf);
}

// Read 16 random bytes — try /dev/urandom first, fall back to std::random_device
static void randomBytes16(uint8_t out[16]) {
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom.is_open()) {
        urandom.read(reinterpret_cast<char*>(out), 16);
        if (urandom.gcount() == 16) return;
    }
    // Fallback: std::random_device
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    for (int i = 0; i < 16; ++i) {
        out[i] = static_cast<uint8_t>(dist(gen));
    }
}

bool UuidModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "v4", "v5", "is_valid", "nil"
    };
    return functions.count(name) > 0;
}

interpreter::NaabVal UuidModule::call(
    const std::string& function_name,
    std::vector<interpreter::NaabVal>& args) {

    if (function_name == "v4") {
        if (!args.empty()) {
            throw std::runtime_error("uuid.v4() takes no arguments");
        }
        uint8_t bytes[16];
        randomBytes16(bytes);
        // Set version 4: high nibble of byte 6 = 0100
        bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
        // Set variant bits: high 2 bits of byte 8 = 10
        bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);
        return interpreter::NaabVal::makeString(formatUUID(bytes));
    }

    if (function_name == "nil") {
        if (!args.empty()) {
            throw std::runtime_error("uuid.nil() takes no arguments");
        }
        return interpreter::NaabVal::makeString(
            "00000000-0000-0000-0000-000000000000");
    }

    if (function_name == "is_valid") {
        if (args.size() != 1) {
            throw std::runtime_error("uuid.is_valid() takes exactly 1 argument");
        }
        if (!args[0].isString()) {
            return interpreter::NaabVal::makeBool(false);
        }
        static const std::regex uuid_pattern(
            "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}"
            "-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"
        );
        return interpreter::NaabVal::makeBool(
            std::regex_match(args[0].asString(), uuid_pattern));
    }

    if (function_name == "v5") {
#ifdef HAS_OPENSSL
        if (args.size() != 2) {
            throw std::runtime_error(
                "uuid.v5() takes exactly 2 arguments: (namespace_uuid, name)\n\n"
                "  Example: uuid.v5(\"6ba7b810-9dad-11d1-80b4-00c04fd430c8\", \"hello\")\n"
            );
        }
        std::string ns_str = args[0].toString();
        std::string name_str = args[1].toString();

        // Parse namespace UUID into 16 bytes
        // Remove dashes: xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
        std::string ns_clean;
        for (char c : ns_str) {
            if (c != '-') ns_clean += c;
        }
        if (ns_clean.size() != 32) {
            throw std::runtime_error("uuid.v5(): invalid namespace UUID: " + ns_str);
        }
        uint8_t ns_bytes[16];
        for (int i = 0; i < 16; ++i) {
            ns_bytes[i] = static_cast<uint8_t>(
                std::stoul(ns_clean.substr(i * 2, 2), nullptr, 16));
        }

        // SHA-1 of namespace bytes + name
        SHA_CTX ctx;
        SHA1_Init(&ctx);
        SHA1_Update(&ctx, ns_bytes, 16);
        SHA1_Update(&ctx, name_str.data(), name_str.size());
        uint8_t digest[20];
        SHA1_Final(digest, &ctx);

        // Copy first 16 bytes, set version=5, set variant
        uint8_t result[16];
        std::memcpy(result, digest, 16);
        result[6] = static_cast<uint8_t>((result[6] & 0x0F) | 0x50); // version 5
        result[8] = static_cast<uint8_t>((result[8] & 0x3F) | 0x80); // variant

        return interpreter::NaabVal::makeString(formatUUID(result));
#else
        (void)args;
        throw std::runtime_error(
            "uuid.v5() requires OpenSSL (not available in this build)\n\n"
            "  Use uuid.v4() for random UUIDs, which has no dependencies.\n"
        );
#endif
    }

    throw std::runtime_error(
        "Unknown uuid function: " + function_name + "\n\n"
        "  Available: v4, v5, is_valid, nil\n"
    );
}

} // namespace stdlib
} // namespace naab
