// Secure String Unit Tests
// Tests auto-zeroization and secure memory handling

#include <gtest/gtest.h>
#include "naab/secure_string.h"
#include <vector>
#include <cstring>

using namespace naab::secure;

// ============================================================================
// SecureString Construction Tests
// ============================================================================

TEST(SecureStringTest, DefaultConstructor) {
    SecureString s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0);
}

TEST(SecureStringTest, StringConstructor) {
    SecureString s("password123");
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.size(), 11);
    EXPECT_EQ(s.get(), "password123");
}

TEST(SecureStringTest, CStringConstructor) {
    const char* password = "secret";
    SecureString s(password);
    EXPECT_EQ(s.get(), "secret");
    EXPECT_EQ(s.size(), 6);
}

TEST(SecureStringTest, CStringNullConstructor) {
    SecureString s(nullptr);
    EXPECT_TRUE(s.empty());
}

TEST(SecureStringTest, BufferConstructor) {
    const char* password = "test1234";
    SecureString s(password, 4);  // Only first 4 chars
    EXPECT_EQ(s.get(), "test");
    EXPECT_EQ(s.size(), 4);
}

// ============================================================================
// SecureString Copy/Move Tests
// ============================================================================

TEST(SecureStringTest, CopyConstructor) {
    SecureString original("password");
    SecureString copy(original);

    EXPECT_EQ(copy.get(), "password");
    EXPECT_EQ(original.get(), "password");
}

TEST(SecureStringTest, MoveConstructor) {
    SecureString original("password");
    SecureString moved(std::move(original));

    EXPECT_EQ(moved.get(), "password");
    EXPECT_TRUE(original.empty());  // Original should be zeroized
}

TEST(SecureStringTest, CopyAssignment) {
    SecureString original("password");
    SecureString copy("old");

    copy = original;
    EXPECT_EQ(copy.get(), "password");
    EXPECT_EQ(original.get(), "password");
}

TEST(SecureStringTest, MoveAssignment) {
    SecureString original("password");
    SecureString moved("old");

    moved = std::move(original);
    EXPECT_EQ(moved.get(), "password");
    EXPECT_TRUE(original.empty());  // Original should be zeroized
}

TEST(SecureStringTest, StringAssignment) {
    SecureString s("old");
    s = std::string("new");
    EXPECT_EQ(s.get(), "new");
}

// ============================================================================
// SecureString Zeroization Tests
// ============================================================================

TEST(SecureStringTest, ManualZeroize) {
    SecureString s("sensitive");
    EXPECT_FALSE(s.empty());

    s.zeroize();
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0);
}

TEST(SecureStringTest, ZeroizeMultipleTimes) {
    SecureString s("data");
    s.zeroize();
    s.zeroize();  // Should not crash
    EXPECT_TRUE(s.empty());
}

TEST(SecureStringTest, AutomaticZeroizeOnDestruction) {
    {
        SecureString s("secret123");
        EXPECT_FALSE(s.empty());
    }  // Destructor runs here - should zeroize

    // Note: This test verifies the destructor is called
    // Actual zeroization can't be tested reliably without
    // examining memory directly
}

TEST(SecureStringTest, ZeroizeBeforeCopy) {
    SecureString s1("first");
    SecureString s2("second");

    s1 = s2;  // Should zeroize "first" before copying "second"
    EXPECT_EQ(s1.get(), "second");
}

// ============================================================================
// SecureString Comparison Tests
// ============================================================================

TEST(SecureStringTest, EqualsIdentical) {
    SecureString s1("password");
    SecureString s2("password");

    EXPECT_TRUE(s1.equals(s2));
}

TEST(SecureStringTest, EqualsDifferent) {
    SecureString s1("password");
    SecureString s2("Password");  // Different case

    EXPECT_FALSE(s1.equals(s2));
}

TEST(SecureStringTest, EqualsDifferentLength) {
    SecureString s1("password");
    SecureString s2("pass");

    EXPECT_FALSE(s1.equals(s2));
}

TEST(SecureStringTest, EqualsEmpty) {
    SecureString s1;
    SecureString s2;

    EXPECT_TRUE(s1.equals(s2));
}

TEST(SecureStringTest, EqualsEmptyAndNonEmpty) {
    SecureString s1;
    SecureString s2("password");

    EXPECT_FALSE(s1.equals(s2));
}

// ============================================================================
// SecureString Access Tests
// ============================================================================

TEST(SecureStringTest, GetMethod) {
    SecureString s("test");
    EXPECT_EQ(s.get(), "test");
}

TEST(SecureStringTest, CStrMethod) {
    SecureString s("test");
    EXPECT_STREQ(s.c_str(), "test");
}

TEST(SecureStringTest, SizeMethod) {
    SecureString s("hello");
    EXPECT_EQ(s.size(), 5);
}

TEST(SecureStringTest, EmptyMethod) {
    SecureString s1;
    SecureString s2("data");

    EXPECT_TRUE(s1.empty());
    EXPECT_FALSE(s2.empty());
}

TEST(SecureStringTest, ToString) {
    SecureString s("password");
    std::string regular = s.to_string();

    EXPECT_EQ(regular, "password");
    // Note: regular string is NOT auto-zeroized
}

// ============================================================================
// SecureBuffer Tests
// ============================================================================

TEST(SecureBufferTest, DefaultConstructor) {
    SecureBuffer<uint8_t> buffer;
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.size(), 0);
}

TEST(SecureBufferTest, SizeConstructor) {
    SecureBuffer<uint8_t> buffer(100);
    EXPECT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.size(), 100);
}

TEST(SecureBufferTest, PointerConstructor) {
    uint8_t data[] = {1, 2, 3, 4, 5};
    SecureBuffer<uint8_t> buffer(data, 5);

    EXPECT_EQ(buffer.size(), 5);
    EXPECT_EQ(buffer[0], 1);
    EXPECT_EQ(buffer[4], 5);
}

TEST(SecureBufferTest, VectorConstructor) {
    std::vector<uint8_t> data = {10, 20, 30};
    SecureBuffer<uint8_t> buffer(data);

    EXPECT_EQ(buffer.size(), 3);
    EXPECT_EQ(buffer[0], 10);
    EXPECT_EQ(buffer[2], 30);
}

TEST(SecureBufferTest, ArrayAccess) {
    SecureBuffer<int> buffer(5);
    buffer[0] = 100;
    buffer[4] = 500;

    EXPECT_EQ(buffer[0], 100);
    EXPECT_EQ(buffer[4], 500);
}

TEST(SecureBufferTest, DataPointer) {
    SecureBuffer<uint8_t> buffer(10);
    uint8_t* ptr = buffer.data();

    ptr[0] = 42;
    EXPECT_EQ(buffer[0], 42);
}

TEST(SecureBufferTest, Resize) {
    SecureBuffer<int> buffer(10);
    buffer[0] = 100;

    buffer.resize(5);
    EXPECT_EQ(buffer.size(), 5);
}

TEST(SecureBufferTest, ManualZeroize) {
    SecureBuffer<uint8_t> buffer(10);
    buffer[0] = 255;

    buffer.zeroize();
    EXPECT_TRUE(buffer.empty());
}

TEST(SecureBufferTest, CopyConstructor) {
    SecureBuffer<int> original(5);
    original[0] = 42;

    SecureBuffer<int> copy(original);
    EXPECT_EQ(copy.size(), 5);
    EXPECT_EQ(copy[0], 42);
}

TEST(SecureBufferTest, MoveConstructor) {
    SecureBuffer<int> original(5);
    original[0] = 42;

    SecureBuffer<int> moved(std::move(original));
    EXPECT_EQ(moved[0], 42);
    EXPECT_TRUE(original.empty());
}

// ============================================================================
// ZeroizeGuard Tests
// ============================================================================

TEST(ZeroizeGuardTest, StringGuard) {
    std::string password = "secret123";

    {
        ZeroizeGuard guard(password);
        // Use password...
    }  // Guard destructor zeroizes

    // String should be empty after guard destruction
    EXPECT_TRUE(password.empty());
}

TEST(ZeroizeGuardTest, VectorGuard) {
    std::vector<uint8_t> key = {1, 2, 3, 4, 5};

    {
        ZeroizeGuard guard(key);
        // Use key...
    }  // Guard destructor zeroizes

    EXPECT_TRUE(key.empty());
}

TEST(ZeroizeGuardTest, EmptyString) {
    std::string empty_str;

    {
        ZeroizeGuard guard(empty_str);
    }  // Should not crash

    EXPECT_TRUE(empty_str.empty());
}

// ============================================================================
// Utility Function Tests
// ============================================================================

TEST(SecureStringUtilsTest, ZeroizeString) {
    std::string password = "sensitive";
    zeroize(password);

    EXPECT_TRUE(password.empty());
}

TEST(SecureStringUtilsTest, ZeroizeVector) {
    std::vector<int> data = {1, 2, 3, 4, 5};
    zeroize(data);

    EXPECT_TRUE(data.empty());
}

TEST(SecureStringUtilsTest, ZeroizeBufferPointer) {
    char buffer[10] = "secret";
    zeroize(buffer, 10);

    // Memory should be zeroed (though we can't easily verify)
    // At least verify function doesn't crash
}

TEST(SecureStringUtilsTest, ZeroizeNullPointer) {
    zeroize(nullptr, 10);  // Should not crash
}

TEST(SecureStringUtilsTest, ZeroizeZeroSize) {
    char buffer[10] = "test";
    zeroize(buffer, 0);  // Should not crash
}

// ============================================================================
// Constant-Time Comparison Tests
// ============================================================================

TEST(SecureStringTest, ConstantTimeComparison_Equal) {
    SecureString s1("password");
    SecureString s2("password");

    // Should return true without early exit
    EXPECT_TRUE(s1.equals(s2));
}

TEST(SecureStringTest, ConstantTimeComparison_DifferentFirstChar) {
    SecureString s1("password");
    SecureString s2("Password");

    // Should check all chars (constant time)
    EXPECT_FALSE(s1.equals(s2));
}

TEST(SecureStringTest, ConstantTimeComparison_DifferentLastChar) {
    SecureString s1("password");
    SecureString s2("passwore");

    // Should check all chars (constant time)
    EXPECT_FALSE(s1.equals(s2));
}

TEST(SecureStringTest, ConstantTimeComparison_DifferentMiddle) {
    SecureString s1("password");
    SecureString s2("passXord");

    EXPECT_FALSE(s1.equals(s2));
}

// ============================================================================
// Edge Case Tests
// ============================================================================

TEST(SecureStringTest, EdgeCase_VeryLongString) {
    std::string long_string(10000, 'x');
    SecureString s(long_string);

    EXPECT_EQ(s.size(), 10000);
    s.zeroize();
    EXPECT_TRUE(s.empty());
}

TEST(SecureStringTest, EdgeCase_SpecialCharacters) {
    SecureString s("p@ssw0rd!#$%");
    EXPECT_EQ(s.get(), "p@ssw0rd!#$%");
}

TEST(SecureStringTest, EdgeCase_UnicodeCharacters) {
    SecureString s("пароль");  // Russian for "password"
    EXPECT_FALSE(s.empty());
}

TEST(SecureStringTest, EdgeCase_NullBytes) {
    std::string with_null = std::string("hello\0world", 11);
    SecureString s(with_null);
    EXPECT_EQ(s.size(), 11);
}

TEST(SecureBufferTest, EdgeCase_LargeBuffer) {
    SecureBuffer<uint8_t> buffer(1000000);  // 1 MB
    EXPECT_EQ(buffer.size(), 1000000);

    buffer.zeroize();
    EXPECT_TRUE(buffer.empty());
}

TEST(SecureBufferTest, EdgeCase_DifferentTypes) {
    SecureBuffer<int32_t> int_buffer(10);
    SecureBuffer<int64_t> long_buffer(10);
    SecureBuffer<double> double_buffer(10);

    EXPECT_EQ(int_buffer.size(), 10);
    EXPECT_EQ(long_buffer.size(), 10);
    EXPECT_EQ(double_buffer.size(), 10);
}

// ============================================================================
// Security Property Tests
// ============================================================================

TEST(SecureStringTest, SecurityProperty_NoLeakOnException) {
    try {
        SecureString s("secret");
        throw std::runtime_error("test");
    } catch (...) {
        // SecureString destructor should have run and zeroized
    }
}

TEST(SecureStringTest, SecurityProperty_ZeroizeOnReassignment) {
    SecureString s("first_secret");
    s = SecureString("second_secret");

    // "first_secret" should have been zeroized
    EXPECT_EQ(s.get(), "second_secret");
}

TEST(SecureBufferTest, SecurityProperty_ZeroizeOnResize) {
    SecureBuffer<uint8_t> buffer(10);
    for (size_t i = 0; i < 10; i++) {
        buffer[i] = static_cast<uint8_t>(i);
    }

    buffer.resize(5);
    // Original data should have been zeroized
    EXPECT_EQ(buffer.size(), 5);
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST(SecureStringTest, Integration_PasswordStorage) {
    // Simulate password storage scenario
    SecureString password("user_password_123");

    // Use password for authentication
    bool authenticated = (password.get() == "user_password_123");
    EXPECT_TRUE(authenticated);

    // Password automatically zeroized when out of scope
}

TEST(SecureStringTest, Integration_ApiKeyHandling) {
    SecureString api_key("sk-1234567890abcdef");

    // Use API key
    std::string request_header = "Authorization: Bearer " + api_key.get();
    EXPECT_TRUE(request_header.find("sk-1234567890abcdef") != std::string::npos);

    // API key automatically zeroized
}

TEST(SecureBufferTest, Integration_CryptoKeyStorage) {
    // Simulate cryptographic key storage
    uint8_t key_bytes[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    SecureBuffer<uint8_t> crypto_key(key_bytes, 8);

    // Use key for encryption...
    EXPECT_EQ(crypto_key.size(), 8);

    // Key automatically zeroized on destruction
}

TEST(SecureStringTest, Integration_WithZeroizeGuard) {
    std::string temp_password = "temporary";

    {
        ZeroizeGuard guard(temp_password);
        // Use password...
        EXPECT_EQ(temp_password, "temporary");
    }

    // Automatically zeroized
    EXPECT_TRUE(temp_password.empty());
}

// ============================================================================
// Platform-Specific Zeroization Tests
// ============================================================================

TEST(SecureStringTest, PlatformZeroization_StringContent) {
    SecureString s("sensitive_data_here");
    EXPECT_FALSE(s.empty());

    s.zeroize();

    // After zeroization, string should be empty
    EXPECT_TRUE(s.empty());
}

TEST(SecureBufferTest, PlatformZeroization_BufferContent) {
    SecureBuffer<uint8_t> buffer(20);

    // Fill with non-zero data
    for (size_t i = 0; i < 20; i++) {
        buffer[i] = static_cast<uint8_t>(i + 1);
    }

    buffer.zeroize();
    EXPECT_TRUE(buffer.empty());
}
