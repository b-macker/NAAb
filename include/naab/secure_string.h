#pragma once
// NAAb Secure String
// Phase 1: Path to 97% - Sensitive Data Zeroization
//
// Automatically zeroizes sensitive data on destruction to prevent:
// - Memory disclosure attacks
// - Core dump leakage
// - Swap file leakage
// - Use-after-free information disclosure


#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <fmt/core.h>

// Platform-specific secure zeroization
#ifdef _WIN32
    #include <windows.h>
    #define SECURE_ZERO(ptr, size) SecureZeroMemory(ptr, size)
#else
    // Use explicit_bzero if available (glibc 2.25+, BSD)
    #if defined(__GLIBC__) && \
        (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
        #define SECURE_ZERO(ptr, size) explicit_bzero(ptr, size)
    #elif defined(__OpenBSD__) || defined(__FreeBSD__)
        #define SECURE_ZERO(ptr, size) explicit_bzero(ptr, size)
    #else
        // Fallback: volatile prevents compiler optimization
        inline void secure_zero_fallback(void* ptr, size_t size) {
            volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
            while (size--) {
                *p++ = 0;
            }
        }
        #define SECURE_ZERO(ptr, size) secure_zero_fallback(ptr, size)
    #endif
#endif

namespace naab {
namespace secure {

// ============================================================================
// SecureString - Auto-zeroizing string for sensitive data
// ============================================================================

/**
 * String class that automatically zeroizes content on destruction
 *
 * Use for passwords, API keys, tokens, and other sensitive data
 *
 * Example:
 *   SecureString password("secret123");
 *   // Use password...
 *   // Automatically zeroized when password goes out of scope
 */
class SecureString {
public:
    // Constructors
    SecureString() = default;

    explicit SecureString(const std::string& str) : data_(str) {}

    explicit SecureString(const char* str) : data_(str ? str : "") {}

    SecureString(const char* str, size_t len) : data_(str, len) {}

    // Copy constructor - copies data
    SecureString(const SecureString& other) : data_(other.data_) {}

    // Move constructor - moves data, zeroizes source
    SecureString(SecureString&& other) noexcept : data_(std::move(other.data_)) {
        other.zeroize();
    }

    // Destructor - automatically zeroizes
    ~SecureString() {
        zeroize();
    }

    // Copy assignment
    SecureString& operator=(const SecureString& other) {
        if (this != &other) {
            zeroize();  // Clear old data first
            data_ = other.data_;
        }
        return *this;
    }

    // Move assignment
    SecureString& operator=(SecureString&& other) noexcept {
        if (this != &other) {
            zeroize();  // Clear old data first
            data_ = std::move(other.data_);
            other.zeroize();
        }
        return *this;
    }

    // String assignment
    SecureString& operator=(const std::string& str) {
        zeroize();
        data_ = str;
        return *this;
    }

    // Access methods
    const std::string& get() const { return data_; }
    const char* c_str() const { return data_.c_str(); }
    size_t size() const { return data_.size(); }
    bool empty() const { return data_.empty(); }

    // Explicitly zeroize (can be called manually)
    void zeroize() {
        if (!data_.empty()) {
            // Zeroize the actual string buffer
            SECURE_ZERO(&data_[0], data_.size());
            data_.clear();
        }
    }

    // Comparison (constant-time to prevent timing attacks)
    bool equals(const SecureString& other) const {
        return constant_time_compare(data_, other.data_);
    }

    // String operations (return regular strings - use with care!)
    std::string to_string() const {
        return data_;  // Warning: result is not auto-zeroized
    }

private:
    std::string data_;

    // Constant-time string comparison (prevents timing attacks)
    static bool constant_time_compare(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) {
            return false;
        }

        volatile uint8_t result = 0;
        for (size_t i = 0; i < a.size(); i++) {
            result |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
        }

        return result == 0;
    }
};

// ============================================================================
// SecureBuffer - Auto-zeroizing buffer for binary data
// ============================================================================

/**
 * Buffer class that automatically zeroizes content on destruction
 *
 * Use for cryptographic keys, tokens, or any sensitive binary data
 */
template<typename T = uint8_t>
class SecureBuffer {
public:
    SecureBuffer() = default;

    explicit SecureBuffer(size_t size) : data_(size) {}

    SecureBuffer(const T* ptr, size_t size) : data_(ptr, ptr + size) {}

    explicit SecureBuffer(const std::vector<T>& vec) : data_(vec) {}

    // Copy constructor
    SecureBuffer(const SecureBuffer& other) : data_(other.data_) {}

    // Move constructor
    SecureBuffer(SecureBuffer&& other) noexcept : data_(std::move(other.data_)) {
        other.zeroize();
    }

    // Destructor
    ~SecureBuffer() {
        zeroize();
    }

    // Copy assignment
    SecureBuffer& operator=(const SecureBuffer& other) {
        if (this != &other) {
            zeroize();
            data_ = other.data_;
        }
        return *this;
    }

    // Move assignment
    SecureBuffer& operator=(SecureBuffer&& other) noexcept {
        if (this != &other) {
            zeroize();
            data_ = std::move(other.data_);
            other.zeroize();
        }
        return *this;
    }

    // Access
    T* data() { return data_.data(); }
    const T* data() const { return data_.data(); }
    size_t size() const { return data_.size(); }
    bool empty() const { return data_.empty(); }

    T& operator[](size_t index) { return data_[index]; }
    const T& operator[](size_t index) const { return data_[index]; }

    // Resize (zeroizes before resizing)
    void resize(size_t new_size) {
        zeroize();
        data_.resize(new_size);
    }

    // Explicitly zeroize
    void zeroize() {
        if (!data_.empty()) {
            SECURE_ZERO(data_.data(), data_.size() * sizeof(T));
            data_.clear();
        }
    }

private:
    std::vector<T> data_;
};

// ============================================================================
// ZeroizeGuard - RAII guard for zeroizing arbitrary data
// ============================================================================

/**
 * RAII guard that zeroizes a string on scope exit
 *
 * Usage:
 *   std::string password = getUserPassword();
 *   ZeroizeGuard guard(password);
 *   // Use password...
 *   // Automatically zeroized when guard goes out of scope
 */
class ZeroizeGuard {
public:
    explicit ZeroizeGuard(std::string& str) : str_(&str), size_(str.size()) {}

    explicit ZeroizeGuard(std::vector<uint8_t>& vec)
        : vec_(&vec), size_(vec.size()) {}

    ~ZeroizeGuard() {
        if (str_ && !str_->empty()) {
            SECURE_ZERO(&(*str_)[0], str_->size());
            str_->clear();
        }

        if (vec_ && !vec_->empty()) {
            SECURE_ZERO(vec_->data(), vec_->size());
            vec_->clear();
        }
    }

    // Non-copyable, non-movable
    ZeroizeGuard(const ZeroizeGuard&) = delete;
    ZeroizeGuard& operator=(const ZeroizeGuard&) = delete;

private:
    std::string* str_ = nullptr;
    std::vector<uint8_t>* vec_ = nullptr;
    size_t size_;
};

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Zeroize a string (free function)
 */
inline void zeroize(std::string& str) {
    if (!str.empty()) {
        SECURE_ZERO(&str[0], str.size());
        str.clear();
    }
}

/**
 * Zeroize a vector
 */
template<typename T>
inline void zeroize(std::vector<T>& vec) {
    if (!vec.empty()) {
        SECURE_ZERO(vec.data(), vec.size() * sizeof(T));
        vec.clear();
    }
}

/**
 * Zeroize a C-style buffer
 */
inline void zeroize(void* ptr, size_t size) {
    if (ptr && size > 0) {
        SECURE_ZERO(ptr, size);
    }
}

/**
 * Create a SecureString from user input
 *
 * Example:
 *   auto password = getSecureInput("Enter password: ");
 *   // password is auto-zeroized
 */
inline SecureString getSecureInput(const std::string& prompt) {
    fmt::print("{}", prompt);
    std::string input;
    std::getline(std::cin, input);

    SecureString result(input);

    // Zeroize the temporary input string
    zeroize(input);

    return result;
}

} // namespace secure
} // namespace naab

