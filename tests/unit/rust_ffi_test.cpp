// Phase 3.1: Rust FFI Integration Tests
// Tests Rust FFI value creation, access, and conversion

#include <gtest/gtest.h>
#include "naab/rust_ffi.h"
#include "naab/interpreter.h"
#include <cstring>

// Forward declaration of conversion helpers
namespace naab {
namespace runtime {
    std::shared_ptr<interpreter::Value> ffiToValue(NaabRustValue* ffi_val);
    NaabRustValue* valueToFfi(const std::shared_ptr<interpreter::Value>& val);
}
}

// Test FFI value creation and access
TEST(RustFFITest, CreateAndGetInt) {
    NaabRustValue* val = naab_rust_value_create_int(42);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(naab_rust_value_get_type(val), NAAB_RUST_TYPE_INT);
    EXPECT_EQ(naab_rust_value_get_int(val), 42);
    naab_rust_value_free(val);
}

TEST(RustFFITest, CreateAndGetDouble) {
    NaabRustValue* val = naab_rust_value_create_double(3.14159);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(naab_rust_value_get_type(val), NAAB_RUST_TYPE_DOUBLE);
    EXPECT_DOUBLE_EQ(naab_rust_value_get_double(val), 3.14159);
    naab_rust_value_free(val);
}

TEST(RustFFITest, CreateAndGetBool) {
    NaabRustValue* val_true = naab_rust_value_create_bool(true);
    ASSERT_NE(val_true, nullptr);
    EXPECT_EQ(naab_rust_value_get_type(val_true), NAAB_RUST_TYPE_BOOL);
    EXPECT_EQ(naab_rust_value_get_bool(val_true), true);
    naab_rust_value_free(val_true);

    NaabRustValue* val_false = naab_rust_value_create_bool(false);
    ASSERT_NE(val_false, nullptr);
    EXPECT_EQ(naab_rust_value_get_bool(val_false), false);
    naab_rust_value_free(val_false);
}

TEST(RustFFITest, CreateAndGetString) {
    const char* test_str = "Hello, Rust FFI!";
    NaabRustValue* val = naab_rust_value_create_string(test_str);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(naab_rust_value_get_type(val), NAAB_RUST_TYPE_STRING);

    const char* result = naab_rust_value_get_string(val);
    ASSERT_NE(result, nullptr);
    EXPECT_STREQ(result, test_str);

    naab_rust_value_free(val);
}

TEST(RustFFITest, CreateVoid) {
    NaabRustValue* val = naab_rust_value_create_void();
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(naab_rust_value_get_type(val), NAAB_RUST_TYPE_VOID);
    naab_rust_value_free(val);
}

// Test type safety - getting wrong type should return default
TEST(RustFFITest, TypeSafety) {
    NaabRustValue* int_val = naab_rust_value_create_int(42);

    // Getting int as wrong types should return defaults
    EXPECT_DOUBLE_EQ(naab_rust_value_get_double(int_val), 0.0);
    EXPECT_EQ(naab_rust_value_get_bool(int_val), false);
    EXPECT_STREQ(naab_rust_value_get_string(int_val), "");

    naab_rust_value_free(int_val);
}

// Test null handling
TEST(RustFFITest, NullHandling) {
    EXPECT_EQ(naab_rust_value_get_int(nullptr), 0);
    EXPECT_DOUBLE_EQ(naab_rust_value_get_double(nullptr), 0.0);
    EXPECT_EQ(naab_rust_value_get_bool(nullptr), false);
    EXPECT_STREQ(naab_rust_value_get_string(nullptr), "");
    EXPECT_EQ(naab_rust_value_get_type(nullptr), NAAB_RUST_TYPE_VOID);

    // Free null should not crash
    naab_rust_value_free(nullptr);
}

// Test conversion: C++ Value -> FFI -> C++ Value
TEST(RustFFITest, RoundTripConversionInt) {
    using namespace naab;

    auto original = std::make_shared<interpreter::Value>(123);
    NaabRustValue* ffi_val = runtime::valueToFfi(original);
    ASSERT_NE(ffi_val, nullptr);

    auto recovered = runtime::ffiToValue(ffi_val);
    ASSERT_NE(recovered, nullptr);
    EXPECT_TRUE(std::holds_alternative<int>(recovered->data));
    EXPECT_EQ(std::get<int>(recovered->data), 123);

    naab_rust_value_free(ffi_val);
}

TEST(RustFFITest, RoundTripConversionDouble) {
    using namespace naab;

    auto original = std::make_shared<interpreter::Value>(2.71828);
    NaabRustValue* ffi_val = runtime::valueToFfi(original);
    ASSERT_NE(ffi_val, nullptr);

    auto recovered = runtime::ffiToValue(ffi_val);
    ASSERT_NE(recovered, nullptr);
    EXPECT_TRUE(std::holds_alternative<double>(recovered->data));
    EXPECT_DOUBLE_EQ(std::get<double>(recovered->data), 2.71828);

    naab_rust_value_free(ffi_val);
}

TEST(RustFFITest, RoundTripConversionBool) {
    using namespace naab;

    auto original = std::make_shared<interpreter::Value>(true);
    NaabRustValue* ffi_val = runtime::valueToFfi(original);
    ASSERT_NE(ffi_val, nullptr);

    auto recovered = runtime::ffiToValue(ffi_val);
    ASSERT_NE(recovered, nullptr);
    EXPECT_TRUE(std::holds_alternative<bool>(recovered->data));
    EXPECT_EQ(std::get<bool>(recovered->data), true);

    naab_rust_value_free(ffi_val);
}

TEST(RustFFITest, RoundTripConversionString) {
    using namespace naab;

    auto original = std::make_shared<interpreter::Value>(std::string("Test string"));
    NaabRustValue* ffi_val = runtime::valueToFfi(original);
    ASSERT_NE(ffi_val, nullptr);

    auto recovered = runtime::ffiToValue(ffi_val);
    ASSERT_NE(recovered, nullptr);
    EXPECT_TRUE(std::holds_alternative<std::string>(recovered->data));
    EXPECT_EQ(std::get<std::string>(recovered->data), "Test string");

    naab_rust_value_free(ffi_val);
}

TEST(RustFFITest, ConversionNullValue) {
    using namespace naab;

    // Null C++ value -> FFI should create void
    NaabRustValue* ffi_val = runtime::valueToFfi(nullptr);
    ASSERT_NE(ffi_val, nullptr);
    EXPECT_EQ(naab_rust_value_get_type(ffi_val), NAAB_RUST_TYPE_VOID);
    naab_rust_value_free(ffi_val);

    // Null FFI value -> C++ should create void value
    auto cpp_val = runtime::ffiToValue(nullptr);
    ASSERT_NE(cpp_val, nullptr);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(cpp_val->data));
}

// Memory safety test - ensure strings are properly freed
TEST(RustFFITest, StringMemoryManagement) {
    const char* long_str = "This is a very long string to test memory management and ensure no leaks";

    for (int i = 0; i < 1000; ++i) {
        NaabRustValue* val = naab_rust_value_create_string(long_str);
        ASSERT_NE(val, nullptr);
        const char* result = naab_rust_value_get_string(val);
        EXPECT_STREQ(result, long_str);
        naab_rust_value_free(val);
    }

    // If we reach here without crashing, memory management is working
    SUCCEED();
}
