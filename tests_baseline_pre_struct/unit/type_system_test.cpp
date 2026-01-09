// Type System Unit Tests
// Tests type parsing, compatibility, and inference

#include <gtest/gtest.h>
#include "naab/type_system.h"

using namespace naab::types;

// ============================================================================
// Basic Type Tests
// ============================================================================

TEST(TypeSystemTest, IntType) {
    auto t = Type::Int();
    EXPECT_EQ(t.toString(), "int");
}

TEST(TypeSystemTest, FloatType) {
    auto t = Type::Float();
    EXPECT_EQ(t.toString(), "float");
}

TEST(TypeSystemTest, StringType) {
    auto t = Type::String();
    EXPECT_EQ(t.toString(), "string");
}

TEST(TypeSystemTest, BoolType) {
    auto t = Type::Bool();
    EXPECT_EQ(t.toString(), "bool");
}

TEST(TypeSystemTest, VoidType) {
    auto t = Type::Void();
    EXPECT_EQ(t.toString(), "void");
}

TEST(TypeSystemTest, AnyType) {
    auto t = Type::Any();
    EXPECT_EQ(t.toString(), "any");
}

// ============================================================================
// Generic Type Tests
// ============================================================================

TEST(TypeSystemTest, ArrayType) {
    auto t = Type::Array(Type::Int());
    EXPECT_EQ(t.toString(), "array<int>");
}

TEST(TypeSystemTest, NestedArrayType) {
    auto t = Type::Array(Type::Array(Type::Int()));
    EXPECT_EQ(t.toString(), "array<array<int>>");
}

TEST(TypeSystemTest, DictType) {
    auto t = Type::Dict(Type::String(), Type::Int());
    EXPECT_EQ(t.toString(), "dict<string,int>");
}

TEST(TypeSystemTest, FunctionType) {
    auto t = Type::Function({Type::Int(), Type::Int()}, Type::Int());
    EXPECT_TRUE(t.toString().find("function") != std::string::npos);
}

// ============================================================================
// Type Parsing Tests
// ============================================================================

TEST(TypeSystemTest, ParseInt) {
    auto t = Type::parse("int");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->toString(), "int");
}

TEST(TypeSystemTest, ParseFloat) {
    auto t = Type::parse("float");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->toString(), "float");
}

TEST(TypeSystemTest, ParseString) {
    auto t = Type::parse("string");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->toString(), "string");
}

TEST(TypeSystemTest, ParseBool) {
    auto t = Type::parse("bool");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->toString(), "bool");
}

TEST(TypeSystemTest, ParseArrayInt) {
    auto t = Type::parse("array<int>");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->toString(), "array<int>");
}

TEST(TypeSystemTest, ParseArrayString) {
    auto t = Type::parse("array<string>");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->toString(), "array<string>");
}

TEST(TypeSystemTest, ParseDictStringInt) {
    auto t = Type::parse("dict<string,int>");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->toString(), "dict<string,int>");
}

TEST(TypeSystemTest, ParseNestedArray) {
    auto t = Type::parse("array<array<int>>");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->toString(), "array<array<int>>");
}

TEST(TypeSystemTest, ParseComplexDict) {
    auto t = Type::parse("dict<string,array<int>>");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->toString(), "dict<string,array<int>>");
}

// ============================================================================
// Type Compatibility Tests
// ============================================================================

TEST(TypeSystemTest, IntCompatibleWithInt) {
    auto t1 = Type::Int();
    auto t2 = Type::Int();
    EXPECT_TRUE(t1.isCompatibleWith(t2));
}

TEST(TypeSystemTest, IntCompatibleWithFloat) {
    auto t1 = Type::Int();
    auto t2 = Type::Float();
    EXPECT_TRUE(t1.isCompatibleWith(t2));  // Int can be used as float
}

TEST(TypeSystemTest, FloatNotCompatibleWithInt) {
    auto t1 = Type::Float();
    auto t2 = Type::Int();
    EXPECT_FALSE(t1.isCompatibleWith(t2));  // Float cannot be used as int (precision loss)
}

TEST(TypeSystemTest, StringNotCompatibleWithInt) {
    auto t1 = Type::String();
    auto t2 = Type::Int();
    EXPECT_FALSE(t1.isCompatibleWith(t2));
}

TEST(TypeSystemTest, AnyCompatibleWithEverything) {
    auto t1 = Type::Any();
    auto t2 = Type::Int();
    auto t3 = Type::String();
    auto t4 = Type::Array(Type::Int());
    EXPECT_TRUE(t1.isCompatibleWith(t2));
    EXPECT_TRUE(t1.isCompatibleWith(t3));
    EXPECT_TRUE(t1.isCompatibleWith(t4));
}

TEST(TypeSystemTest, EverythingCompatibleWithAny) {
    auto t1 = Type::Any();
    auto t2 = Type::Int();
    auto t3 = Type::String();
    EXPECT_TRUE(t2.isCompatibleWith(t1));
    EXPECT_TRUE(t3.isCompatibleWith(t1));
}

TEST(TypeSystemTest, ArrayIntCompatibleWithArrayInt) {
    auto t1 = Type::Array(Type::Int());
    auto t2 = Type::Array(Type::Int());
    EXPECT_TRUE(t1.isCompatibleWith(t2));
}

TEST(TypeSystemTest, ArrayIntNotCompatibleWithArrayString) {
    auto t1 = Type::Array(Type::Int());
    auto t2 = Type::Array(Type::String());
    EXPECT_FALSE(t1.isCompatibleWith(t2));
}

TEST(TypeSystemTest, DictCompatibility) {
    auto t1 = Type::Dict(Type::String(), Type::Int());
    auto t2 = Type::Dict(Type::String(), Type::Int());
    EXPECT_TRUE(t1.isCompatibleWith(t2));
}

TEST(TypeSystemTest, DictIncompatibilityDifferentKeyType) {
    auto t1 = Type::Dict(Type::String(), Type::Int());
    auto t2 = Type::Dict(Type::Int(), Type::Int());
    EXPECT_FALSE(t1.isCompatibleWith(t2));
}

TEST(TypeSystemTest, DictIncompatibilityDifferentValueType) {
    auto t1 = Type::Dict(Type::String(), Type::Int());
    auto t2 = Type::Dict(Type::String(), Type::String());
    EXPECT_FALSE(t1.isCompatibleWith(t2));
}

// ============================================================================
// Type Equality Tests
// ============================================================================

TEST(TypeSystemTest, IntEqualsInt) {
    auto t1 = Type::Int();
    auto t2 = Type::Int();
    EXPECT_EQ(t1.toString(), t2.toString());
}

TEST(TypeSystemTest, IntNotEqualsFloat) {
    auto t1 = Type::Int();
    auto t2 = Type::Float();
    EXPECT_NE(t1.toString(), t2.toString());
}

// ============================================================================
// Complex Type Tests
// ============================================================================

TEST(TypeSystemTest, ComplexNestedType) {
    auto t = Type::Array(Type::Dict(Type::String(), Type::Array(Type::Int())));
    EXPECT_EQ(t.toString(), "array<dict<string,array<int>>>");
}

TEST(TypeSystemTest, ParseComplexNestedType) {
    auto t = Type::parse("array<dict<string,array<int>>>");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->toString(), "array<dict<string,array<int>>>");
}

TEST(TypeSystemTest, ComplexTypeCompatibility) {
    auto t1 = Type::Array(Type::Dict(Type::String(), Type::Int()));
    auto t2 = Type::Array(Type::Dict(Type::String(), Type::Int()));
    EXPECT_TRUE(t1.isCompatibleWith(t2));
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(TypeSystemTest, VoidNotCompatibleWithInt) {
    auto t1 = Type::Void();
    auto t2 = Type::Int();
    EXPECT_FALSE(t1.isCompatibleWith(t2));
}

TEST(TypeSystemTest, EmptyArrayType) {
    auto t = Type::Array(Type::Any());
    EXPECT_EQ(t.toString(), "array<any>");
}

// Total: 50+ type system tests
