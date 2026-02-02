//
// NAAb Standard Library - Crypto Module
// Complete implementation with 14 cryptographic functions
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include <string>
#include <vector>
#include <unordered_set>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

// OpenSSL headers - if not available, functions will throw errors
#ifdef __has_include
#  if __has_include(<openssl/md5.h>)
#    include <openssl/md5.h>
#    include <openssl/sha.h>
#    include <openssl/evp.h>
#    define HAS_OPENSSL
#  endif
#endif

namespace naab {
namespace stdlib {

// Forward declarations
static std::string getString(const std::shared_ptr<interpreter::Value>& val);
static int getInt(const std::shared_ptr<interpreter::Value>& val);
static std::shared_ptr<interpreter::Value> makeString(const std::string& s);
static std::shared_ptr<interpreter::Value> makeInt(int i);
static std::shared_ptr<interpreter::Value> makeBool(bool b);
static std::string base64_encode(const std::string& input);
static std::string base64_decode(const std::string& input);
static std::string hex_encode(const std::string& input);
static std::string hex_decode(const std::string& input);
static std::string generate_random_bytes(size_t length);
static std::string hash_md5(const std::string& input);
static std::string hash_sha1(const std::string& input);
static std::string hash_sha256(const std::string& input);
static std::string hash_sha512(const std::string& input);

bool CryptoModule::hasFunction(const std::string& name) const {
    static const std::unordered_set<std::string> functions = {
        "md5", "sha1", "sha256", "sha512",
        "base64_encode", "base64_decode", "hex_encode", "hex_decode",
        "random_bytes", "random_string", "random_int",
        "compare_digest", "generate_token", "hash_password"
    };
    return functions.count(name) > 0;
}

std::shared_ptr<interpreter::Value> CryptoModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Function 1: md5
    if (function_name == "md5") {
        if (args.size() != 1) {
            throw std::runtime_error("md5() takes exactly 1 argument");
        }
        std::string text = getString(args[0]);
        return makeString(hash_md5(text));
    }

    // Function 2: sha1
    if (function_name == "sha1") {
        if (args.size() != 1) {
            throw std::runtime_error("sha1() takes exactly 1 argument");
        }
        std::string text = getString(args[0]);
        return makeString(hash_sha1(text));
    }

    // Function 3: sha256
    if (function_name == "sha256") {
        if (args.size() != 1) {
            throw std::runtime_error("sha256() takes exactly 1 argument");
        }
        std::string text = getString(args[0]);
        return makeString(hash_sha256(text));
    }

    // Function 4: sha512
    if (function_name == "sha512") {
        if (args.size() != 1) {
            throw std::runtime_error("sha512() takes exactly 1 argument");
        }
        std::string text = getString(args[0]);
        return makeString(hash_sha512(text));
    }

    // Function 5: base64_encode
    if (function_name == "base64_encode") {
        if (args.size() != 1) {
            throw std::runtime_error("base64_encode() takes exactly 1 argument");
        }
        std::string text = getString(args[0]);
        return makeString(base64_encode(text));
    }

    // Function 6: base64_decode
    if (function_name == "base64_decode") {
        if (args.size() != 1) {
            throw std::runtime_error("base64_decode() takes exactly 1 argument");
        }
        std::string text = getString(args[0]);
        return makeString(base64_decode(text));
    }

    // Function 7: hex_encode
    if (function_name == "hex_encode") {
        if (args.size() != 1) {
            throw std::runtime_error("hex_encode() takes exactly 1 argument");
        }
        std::string text = getString(args[0]);
        return makeString(hex_encode(text));
    }

    // Function 8: hex_decode
    if (function_name == "hex_decode") {
        if (args.size() != 1) {
            throw std::runtime_error("hex_decode() takes exactly 1 argument");
        }
        std::string text = getString(args[0]);
        return makeString(hex_decode(text));
    }

    // Function 9: random_bytes
    if (function_name == "random_bytes") {
        if (args.size() != 1) {
            throw std::runtime_error("random_bytes() takes exactly 1 argument");
        }
        int length = getInt(args[0]);
        if (length < 0 || length > 10000) {
            throw std::runtime_error("random_bytes() length must be between 0 and 10000");
        }
        return makeString(generate_random_bytes(length));
    }

    // Function 10: random_string
    if (function_name == "random_string") {
        if (args.size() != 1) {
            throw std::runtime_error("random_string() takes exactly 1 argument");
        }
        int length = getInt(args[0]);
        if (length < 0 || length > 10000) {
            throw std::runtime_error("random_string() length must be between 0 and 10000");
        }

        static const char charset[] =
            "0123456789"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz";

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

        std::string result;
        for (int i = 0; i < length; ++i) {
            result += charset[dis(gen)];
        }
        return makeString(result);
    }

    // Function 11: random_int
    if (function_name == "random_int") {
        if (args.size() != 2) {
            throw std::runtime_error("random_int() takes exactly 2 arguments (min, max)");
        }
        int min = getInt(args[0]);
        int max = getInt(args[1]);

        if (min > max) {
            throw std::runtime_error("random_int() min must be <= max");
        }

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(min, max);
        return makeInt(dis(gen));
    }

    // Function 12: compare_digest - Constant-time string comparison
    if (function_name == "compare_digest") {
        if (args.size() != 2) {
            throw std::runtime_error("compare_digest() takes exactly 2 arguments");
        }
        std::string a = getString(args[0]);
        std::string b = getString(args[1]);

        if (a.length() != b.length()) {
            return makeBool(false);
        }

        // Constant-time comparison
        volatile unsigned char result = 0;
        for (size_t i = 0; i < a.length(); ++i) {
            result |= a[i] ^ b[i];
        }
        return makeBool(result == 0);
    }

    // Function 13: generate_token
    if (function_name == "generate_token") {
        int length = 32;  // Default length
        if (args.size() == 1) {
            length = getInt(args[0]);
        } else if (args.size() > 1) {
            throw std::runtime_error("generate_token() takes 0 or 1 argument (length?)");
        }

        if (length < 1 || length > 1000) {
            throw std::runtime_error("generate_token() length must be between 1 and 1000");
        }

        // Generate random bytes and hex encode
        std::string random = generate_random_bytes(length);
        return makeString(hex_encode(random));
    }

    // Function 14: hash_password - Simple password hashing using SHA256
    if (function_name == "hash_password") {
        if (args.size() != 1) {
            throw std::runtime_error("hash_password() takes exactly 1 argument");
        }
        std::string password = getString(args[0]);

        // Simple SHA256 hash (in production, use bcrypt or argon2)
        // Note: This is for compatibility; proper password hashing needs salt + iterations
        std::string hashed = hash_sha256(password);
        return makeString(hashed);
    }

    throw std::runtime_error("Unknown function: " + function_name);
}

// Helper functions
static std::string getString(const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([](auto&& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return arg;
        } else {
            throw std::runtime_error("Expected string value");
        }
    }, val->data);
}

static int getInt(const std::shared_ptr<interpreter::Value>& val) {
    return std::visit([](auto&& arg) -> int {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int>) {
            return arg;
        } else if constexpr (std::is_same_v<T, double>) {
            return static_cast<int>(arg);
        } else {
            throw std::runtime_error("Expected integer value");
        }
    }, val->data);
}

static std::shared_ptr<interpreter::Value> makeString(const std::string& s) {
    return std::make_shared<interpreter::Value>(s);
}

static std::shared_ptr<interpreter::Value> makeInt(int i) {
    return std::make_shared<interpreter::Value>(i);
}

static std::shared_ptr<interpreter::Value> makeBool(bool b) {
    return std::make_shared<interpreter::Value>(b);
}

static std::string base64_encode(const std::string& input) {
    static const char* base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string output;
    int val = 0;
    int valb = -6;

    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            output.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }

    if (valb > -6) {
        output.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    }

    while (output.size() % 4) {
        output.push_back('=');
    }

    return output;
}

static std::string base64_decode(const std::string& input) {
    // Validate input characters
    for (unsigned char c : input) {
        if (c == '=') continue;  // Padding is allowed
        if (!std::isalnum(c) && c != '+' && c != '/') {
            throw std::runtime_error("base64_decode() invalid character in input: " + std::string(1, c));
        }
    }

    static const int decode_table[128] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
    };

    std::string output;
    int val = 0;
    int valb = -8;

    for (unsigned char c : input) {
        if (c == '=') break;
        if (c >= 128 || decode_table[c] == -1) continue;

        val = (val << 6) + decode_table[c];
        valb += 6;

        if (valb >= 0) {
            output.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }

    return output;
}

static std::string hex_encode(const std::string& input) {
    std::ostringstream oss;
    for (unsigned char c : input) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    }
    return oss.str();
}

static std::string hex_decode(const std::string& input) {
    // Validate input length is even
    if (input.length() % 2 != 0) {
        throw std::runtime_error("hex_decode() input length must be even (got " +
                               std::to_string(input.length()) + ")");
    }

    // Validate all characters are hex digits
    for (char c : input) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            throw std::runtime_error("hex_decode() invalid hex character: " + std::string(1, c));
        }
    }

    std::string output;
    for (size_t i = 0; i < input.length(); i += 2) {
        std::string byte = input.substr(i, 2);
        char chr = (char)strtol(byte.c_str(), nullptr, 16);
        output.push_back(chr);
    }
    return output;
}

static std::string generate_random_bytes(size_t length) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    std::string result;
    for (size_t i = 0; i < length; ++i) {
        result += static_cast<char>(dis(gen));
    }
    return result;
}

#ifdef HAS_OPENSSL
static std::string hash_md5(const std::string& input) {
    unsigned char hash[MD5_DIGEST_LENGTH];

    // Use EVP API instead of deprecated MD5() function
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, input.c_str(), input.length());
    unsigned int length = 0;
    EVP_DigestFinal_ex(ctx, hash, &length);
    EVP_MD_CTX_free(ctx);

    return hex_encode(std::string(reinterpret_cast<char*>(hash), MD5_DIGEST_LENGTH));
}

static std::string hash_sha1(const std::string& input) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(input.c_str()), input.length(), hash);
    return hex_encode(std::string(reinterpret_cast<char*>(hash), SHA_DIGEST_LENGTH));
}

static std::string hash_sha256(const std::string& input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.c_str()), input.length(), hash);
    return hex_encode(std::string(reinterpret_cast<char*>(hash), SHA256_DIGEST_LENGTH));
}

static std::string hash_sha512(const std::string& input) {
    unsigned char hash[SHA512_DIGEST_LENGTH];
    SHA512(reinterpret_cast<const unsigned char*>(input.c_str()), input.length(), hash);
    return hex_encode(std::string(reinterpret_cast<char*>(hash), SHA512_DIGEST_LENGTH));
}
#else
// Fallback implementations without OpenSSL - throw errors
static std::string hash_md5(const std::string& input) {
    throw std::runtime_error("MD5 hashing requires OpenSSL - not available");
}

static std::string hash_sha1(const std::string& input) {
    throw std::runtime_error("SHA1 hashing requires OpenSSL - not available");
}

static std::string hash_sha256(const std::string& input) {
    throw std::runtime_error("SHA256 hashing requires OpenSSL - not available");
}

static std::string hash_sha512(const std::string& input) {
    throw std::runtime_error("SHA512 hashing requires OpenSSL - not available");
}
#endif

} // namespace stdlib
} // namespace naab
