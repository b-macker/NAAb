// Unit tests for FFI Callback Validator
// Phase 1 Item 9: FFI Callback Safety

#include <gtest/gtest.h>
#include "naab/ffi_callback_validator.h"
#include "naab/value.h"
#include "naab/ast.h"

using namespace naab::ffi;
using namespace naab::interpreter;
using namespace naab::ast;

class FFICallbackValidatorTest : public ::testing::Test {
protected:
    // Helper to create test values
    Value makeInt(int val) { return Value(val); }
    Value makeFloat(double val) { return Value(val); }
    Value makeString(const std::string& val) { return Value(val); }
    Value makeBool(bool val) { return Value(val); }
};

//=============================================================================
// Pointer Validation Tests
//=============================================================================

TEST_F(FFICallbackValidatorTest, RejectsNullPointer) {
    EXPECT_FALSE(CallbackValidator::validatePointer(nullptr));
}

TEST_F(FFICallbackValidatorTest, AcceptsValidPointer) {
    int dummy = 42;
    EXPECT_TRUE(CallbackValidator::validatePointer(&dummy));
}

//=============================================================================
// Argument Count Validation Tests
//=============================================================================

TEST_F(FFICallbackValidatorTest, ValidatesCorrectArgumentCount) {
    EXPECT_TRUE(CallbackValidator::validateArgumentCount(3, 3));
    EXPECT_TRUE(CallbackValidator::validateArgumentCount(0, 0));
    EXPECT_TRUE(CallbackValidator::validateArgumentCount(10, 10));
}

TEST_F(FFICallbackValidatorTest, RejectsIncorrectArgumentCount) {
    EXPECT_FALSE(CallbackValidator::validateArgumentCount(2, 3));
    EXPECT_FALSE(CallbackValidator::validateArgumentCount(5, 4));
    EXPECT_FALSE(CallbackValidator::validateArgumentCount(0, 1));
}

//=============================================================================
// Type Matching Tests
//=============================================================================

TEST_F(FFICallbackValidatorTest, MatchesIntType) {
    Value val = makeInt(42);
    Type type = Type::makeInt();
    EXPECT_TRUE(CallbackValidator::valueMatchesType(val, type));
}

TEST_F(FFICallbackValidatorTest, MatchesFloatType) {
    Value val = makeFloat(3.14);
    Type type = Type::makeFloat();
    EXPECT_TRUE(CallbackValidator::valueMatchesType(val, type));
}

TEST_F(FFICallbackValidatorTest, MatchesStringType) {
    Value val = makeString("test");
    Type type = Type::makeString();
    EXPECT_TRUE(CallbackValidator::valueMatchesType(val, type));
}

TEST_F(FFICallbackValidatorTest, MatchesBoolType) {
    Value val = makeBool(true);
    Type type = Type::makeBool();
    EXPECT_TRUE(CallbackValidator::valueMatchesType(val, type));
}

TEST_F(FFICallbackValidatorTest, AnyTypeAcceptsEverything) {
    Type any_type = Type::makeAny();

    EXPECT_TRUE(CallbackValidator::valueMatchesType(makeInt(42), any_type));
    EXPECT_TRUE(CallbackValidator::valueMatchesType(makeFloat(3.14), any_type));
    EXPECT_TRUE(CallbackValidator::valueMatchesType(makeString("test"), any_type));
    EXPECT_TRUE(CallbackValidator::valueMatchesType(makeBool(true), any_type));
}

TEST_F(FFICallbackValidatorTest, RejectsTypeMismatch) {
    Value int_val = makeInt(42);
    Type string_type = Type::makeString();

    EXPECT_FALSE(CallbackValidator::valueMatchesType(int_val, string_type));
}

//=============================================================================
// Signature Validation Tests
//=============================================================================

TEST_F(FFICallbackValidatorTest, ValidatesCorrectSignature) {
    std::vector<Value> args = {makeInt(42), makeString("test"), makeBool(true)};
    std::vector<Type> types = {Type::makeInt(), Type::makeString(), Type::makeBool()};

    EXPECT_TRUE(CallbackValidator::validateSignature(args, types));
}

TEST_F(FFICallbackValidatorTest, RejectsSignatureMismatch) {
    std::vector<Value> args = {makeInt(42), makeInt(100)};
    std::vector<Type> types = {Type::makeInt(), Type::makeString()};

    EXPECT_FALSE(CallbackValidator::validateSignature(args, types));
}

TEST_F(FFICallbackValidatorTest, ValidatesEmptySignature) {
    std::vector<Value> args;
    std::vector<Type> types;

    EXPECT_TRUE(CallbackValidator::validateSignature(args, types));
}

//=============================================================================
// Return Type Validation Tests
//=============================================================================

TEST_F(FFICallbackValidatorTest, ValidatesCorrectReturnType) {
    Value return_val = makeInt(42);
    Type expected = Type::makeInt();

    EXPECT_TRUE(CallbackValidator::validateReturnType(return_val, expected));
}

TEST_F(FFICallbackValidatorTest, RejectsIncorrectReturnType) {
    Value return_val = makeString("test");
    Type expected = Type::makeInt();

    EXPECT_FALSE(CallbackValidator::validateReturnType(return_val, expected));
}

//=============================================================================
// Exception Boundary Tests
//=============================================================================

TEST_F(FFICallbackValidatorTest, CatchesStdException) {
    auto throwing_callback = []() -> Value {
        throw std::runtime_error("test error");
    };

    auto wrapped = CallbackValidator::wrapCallback(throwing_callback, "test");
    auto result = wrapped();

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_type, "std::exception");
    EXPECT_EQ(result.error_message, "test error");
}

TEST_F(FFICallbackValidatorTest, CatchesUnknownException) {
    auto throwing_callback = []() -> Value {
        throw 42;  // Throw non-std::exception
    };

    auto wrapped = CallbackValidator::wrapCallback(throwing_callback, "test");
    auto result = wrapped();

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_type, "unknown_exception");
}

TEST_F(FFICallbackValidatorTest, SuccessfulCallbackReturnsValue) {
    auto good_callback = []() -> Value {
        return Value(42);
    };

    auto wrapped = CallbackValidator::wrapCallback(good_callback, "test");
    auto result = wrapped();

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.value.toInt(), 42);
}

//=============================================================================
// CallbackValidationGuard Tests
//=============================================================================

TEST_F(FFICallbackValidatorTest, GuardRejectsNullPointer) {
    std::vector<Value> args;
    std::vector<Type> types;

    CallbackValidationGuard guard(nullptr, args, types, "test");

    EXPECT_FALSE(guard.isValid());
    EXPECT_FALSE(guard.getError().empty());
}

TEST_F(FFICallbackValidatorTest, GuardAcceptsValidCallback) {
    int dummy = 42;
    std::vector<Value> args = {makeInt(42)};
    std::vector<Type> types = {Type::makeInt()};

    CallbackValidationGuard guard(&dummy, args, types, "test");

    EXPECT_TRUE(guard.isValid());
    EXPECT_TRUE(guard.getError().empty());
}

TEST_F(FFICallbackValidatorTest, GuardDetectsSignatureMismatch) {
    int dummy = 42;
    std::vector<Value> args = {makeString("test")};
    std::vector<Type> types = {Type::makeInt()};

    CallbackValidationGuard guard(&dummy, args, types, "test");

    EXPECT_FALSE(guard.isValid());
    EXPECT_FALSE(guard.getError().empty());
}

//=============================================================================
// Type Name Tests
//=============================================================================

TEST_F(FFICallbackValidatorTest, GetTypeNameReturnsCorrectNames) {
    EXPECT_EQ(CallbackValidator::getTypeName(Type::makeInt()), "int");
    EXPECT_EQ(CallbackValidator::getTypeName(Type::makeFloat()), "float");
    EXPECT_EQ(CallbackValidator::getTypeName(Type::makeString()), "string");
    EXPECT_EQ(CallbackValidator::getTypeName(Type::makeBool()), "bool");
    EXPECT_EQ(CallbackValidator::getTypeName(Type::makeAny()), "any");
}

TEST_F(FFICallbackValidatorTest, GetValueTypeNameReturnsCorrectNames) {
    EXPECT_EQ(CallbackValidator::getValueTypeName(makeInt(42)), "int");
    EXPECT_EQ(CallbackValidator::getValueTypeName(makeFloat(3.14)), "float");
    EXPECT_EQ(CallbackValidator::getValueTypeName(makeString("test")), "string");
    EXPECT_EQ(CallbackValidator::getValueTypeName(makeBool(true)), "bool");
}
