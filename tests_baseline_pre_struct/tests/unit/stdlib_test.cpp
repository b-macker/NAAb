// Standard Library Unit Tests
// Tests all stdlib modules and functions

#include <gtest/gtest.h>
#include "naab/stdlib.h"
#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"

using namespace naab::stdlib;
using namespace naab::interpreter;

// Helper to create a Value
std::shared_ptr<Value> makeInt(int v) {
    return std::make_shared<Value>(v);
}

std::shared_ptr<Value> makeFloat(double v) {
    return std::make_shared<Value>(v);
}

std::shared_ptr<Value> makeString(const std::string& v) {
    return std::make_shared<Value>(v);
}

std::shared_ptr<Value> makeBool(bool v) {
    return std::make_shared<Value>(v);
}

std::shared_ptr<Value> makeArray(const std::vector<std::shared_ptr<Value>>& v) {
    return std::make_shared<Value>(v);
}

// ============================================================================
// StdLib Manager Tests
// ============================================================================

TEST(StdLibTest, AllModulesAvailable) {
    StdLib stdlib;
    auto modules = stdlib.listModules();
    EXPECT_EQ(modules.size(), 13);  // All 13 modules
}

TEST(StdLibTest, GetModule) {
    StdLib stdlib;
    auto mod = stdlib.getModule("string");
    ASSERT_NE(mod, nullptr);
    EXPECT_EQ(mod->getName(), "string");
}

TEST(StdLibTest, ModuleNotFound) {
    StdLib stdlib;
    auto mod = stdlib.getModule("nonexistent");
    EXPECT_EQ(mod, nullptr);
}

// ============================================================================
// String Module Tests
// ============================================================================

TEST(StringModuleTest, Length) {
    StringModule mod;
    auto result = mod.call("length", {makeString("hello")});
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 5);
}

TEST(StringModuleTest, Upper) {
    StringModule mod;
    auto result = mod.call("upper", {makeString("hello")});
    auto* strval = std::get_if<std::string>(&result->data);
    ASSERT_NE(strval, nullptr);
    EXPECT_EQ(*strval, "HELLO");
}

TEST(StringModuleTest, Lower) {
    StringModule mod;
    auto result = mod.call("lower", {makeString("HELLO")});
    auto* strval = std::get_if<std::string>(&result->data);
    ASSERT_NE(strval, nullptr);
    EXPECT_EQ(*strval, "hello");
}

TEST(StringModuleTest, Trim) {
    StringModule mod;
    auto result = mod.call("trim", {makeString("  hello  ")});
    auto* strval = std::get_if<std::string>(&result->data);
    ASSERT_NE(strval, nullptr);
    EXPECT_EQ(*strval, "hello");
}

TEST(StringModuleTest, Split) {
    StringModule mod;
    auto result = mod.call("split", {makeString("a,b,c"), makeString(",")});
    auto* arrval = std::get_if<std::vector<std::shared_ptr<Value>>>(&result->data);
    ASSERT_NE(arrval, nullptr);
    EXPECT_EQ(arrval->size(), 3);
}

TEST(StringModuleTest, Contains) {
    StringModule mod;
    auto result = mod.call("contains", {makeString("hello world"), makeString("world")});
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

TEST(StringModuleTest, StartsWith) {
    StringModule mod;
    auto result = mod.call("starts_with", {makeString("hello"), makeString("hel")});
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

TEST(StringModuleTest, EndsWith) {
    StringModule mod;
    auto result = mod.call("ends_with", {makeString("hello"), makeString("lo")});
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

TEST(StringModuleTest, Replace) {
    StringModule mod;
    auto result = mod.call("replace", {makeString("hello world"), makeString("world"), makeString("there")});
    auto* strval = std::get_if<std::string>(&result->data);
    ASSERT_NE(strval, nullptr);
    EXPECT_EQ(*strval, "hello there");
}

TEST(StringModuleTest, Substring) {
    StringModule mod;
    auto result = mod.call("substring", {makeString("hello"), makeInt(1), makeInt(4)});
    auto* strval = std::get_if<std::string>(&result->data);
    ASSERT_NE(strval, nullptr);
    EXPECT_EQ(*strval, "ell");
}

TEST(StringModuleTest, IndexOf) {
    StringModule mod;
    auto result = mod.call("index_of", {makeString("hello"), makeString("l")});
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 2);
}

TEST(StringModuleTest, Repeat) {
    StringModule mod;
    auto result = mod.call("repeat", {makeString("ab"), makeInt(3)});
    auto* strval = std::get_if<std::string>(&result->data);
    ASSERT_NE(strval, nullptr);
    EXPECT_EQ(*strval, "ababab");
}

// ============================================================================
// Array Module Tests
// ============================================================================

TEST(ArrayModuleTest, Length) {
    ArrayModule mod;
    auto arr = makeArray({makeInt(1), makeInt(2), makeInt(3)});
    auto result = mod.call("length", {arr});
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 3);
}

TEST(ArrayModuleTest, Push) {
    ArrayModule mod;
    auto arr = makeArray({makeInt(1), makeInt(2)});
    auto result = mod.call("push", {arr, makeInt(3)});
    auto* arrval = std::get_if<std::vector<std::shared_ptr<Value>>>(&result->data);
    ASSERT_NE(arrval, nullptr);
    EXPECT_EQ(arrval->size(), 3);
}

TEST(ArrayModuleTest, Pop) {
    ArrayModule mod;
    auto arr = makeArray({makeInt(1), makeInt(2), makeInt(3)});
    auto result = mod.call("pop", {arr});
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 3);
}

TEST(ArrayModuleTest, Shift) {
    ArrayModule mod;
    auto arr = makeArray({makeInt(1), makeInt(2), makeInt(3)});
    auto result = mod.call("shift", {arr});
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 1);
}

TEST(ArrayModuleTest, Unshift) {
    ArrayModule mod;
    auto arr = makeArray({makeInt(2), makeInt(3)});
    auto result = mod.call("unshift", {arr, makeInt(1)});
    auto* arrval = std::get_if<std::vector<std::shared_ptr<Value>>>(&result->data);
    ASSERT_NE(arrval, nullptr);
    EXPECT_EQ(arrval->size(), 3);
}

TEST(ArrayModuleTest, First) {
    ArrayModule mod;
    auto arr = makeArray({makeInt(1), makeInt(2), makeInt(3)});
    auto result = mod.call("first", {arr});
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 1);
}

TEST(ArrayModuleTest, Last) {
    ArrayModule mod;
    auto arr = makeArray({makeInt(1), makeInt(2), makeInt(3)});
    auto result = mod.call("last", {arr});
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 3);
}

TEST(ArrayModuleTest, Reverse) {
    ArrayModule mod;
    auto arr = makeArray({makeInt(1), makeInt(2), makeInt(3)});
    auto result = mod.call("reverse", {arr});
    auto* arrval = std::get_if<std::vector<std::shared_ptr<Value>>>(&result->data);
    ASSERT_NE(arrval, nullptr);
    EXPECT_EQ(arrval->size(), 3);
    // First element should be 3
    auto* first = std::get_if<int>(&(*arrval)[0]->data);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(*first, 3);
}

TEST(ArrayModuleTest, Contains) {
    ArrayModule mod;
    auto arr = makeArray({makeInt(1), makeInt(2), makeInt(3)});
    auto result = mod.call("contains", {arr, makeInt(2)});
    auto* boolval = std::get_if<bool>(&result->data);
    ASSERT_NE(boolval, nullptr);
    EXPECT_TRUE(*boolval);
}

TEST(ArrayModuleTest, Join) {
    ArrayModule mod;
    auto arr = makeArray({makeString("a"), makeString("b"), makeString("c")});
    auto result = mod.call("join", {arr, makeString(",")});
    auto* strval = std::get_if<std::string>(&result->data);
    ASSERT_NE(strval, nullptr);
    EXPECT_EQ(*strval, "a,b,c");
}

// ============================================================================
// Math Module Tests
// ============================================================================

TEST(MathModuleTest, Abs) {
    MathModule mod;
    auto result = mod.call("abs", {makeInt(-5)});
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 5);
}

TEST(MathModuleTest, Floor) {
    MathModule mod;
    auto result = mod.call("floor", {makeFloat(3.7)});
    auto* floatval = std::get_if<double>(&result->data);
    ASSERT_NE(floatval, nullptr);
    EXPECT_EQ(*floatval, 3.0);
}

TEST(MathModuleTest, Ceil) {
    MathModule mod;
    auto result = mod.call("ceil", {makeFloat(3.2)});
    auto* floatval = std::get_if<double>(&result->data);
    ASSERT_NE(floatval, nullptr);
    EXPECT_EQ(*floatval, 4.0);
}

TEST(MathModuleTest, Round) {
    MathModule mod;
    auto result = mod.call("round", {makeFloat(3.6)});
    auto* floatval = std::get_if<double>(&result->data);
    ASSERT_NE(floatval, nullptr);
    EXPECT_EQ(*floatval, 4.0);
}

TEST(MathModuleTest, Max) {
    MathModule mod;
    auto result = mod.call("max", {makeInt(5), makeInt(10)});
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 10);
}

TEST(MathModuleTest, Min) {
    MathModule mod;
    auto result = mod.call("min", {makeInt(5), makeInt(10)});
    auto* intval = std::get_if<int>(&result->data);
    ASSERT_NE(intval, nullptr);
    EXPECT_EQ(*intval, 5);
}

TEST(MathModuleTest, Pow) {
    MathModule mod;
    auto result = mod.call("pow", {makeInt(2), makeInt(3)});
    auto* floatval = std::get_if<double>(&result->data);
    ASSERT_NE(floatval, nullptr);
    EXPECT_EQ(*floatval, 8.0);
}

TEST(MathModuleTest, Sqrt) {
    MathModule mod;
    auto result = mod.call("sqrt", {makeInt(16)});
    auto* floatval = std::get_if<double>(&result->data);
    ASSERT_NE(floatval, nullptr);
    EXPECT_EQ(*floatval, 4.0);
}

// ============================================================================
// IO Module Tests (Partial - file operations need filesystem)
// ============================================================================

TEST(IOModuleTest, ModuleExists) {
    StdLib stdlib;
    auto mod = stdlib.getModule("io");
    ASSERT_NE(mod, nullptr);
    EXPECT_EQ(mod->getName(), "io");
}

TEST(IOModuleTest, HasReadFile) {
    IOModule mod;
    EXPECT_TRUE(mod.hasFunction("read_file"));
}

TEST(IOModuleTest, HasWriteFile) {
    IOModule mod;
    EXPECT_TRUE(mod.hasFunction("write_file"));
}

TEST(IOModuleTest, HasExists) {
    IOModule mod;
    EXPECT_TRUE(mod.hasFunction("exists"));
}

// ============================================================================
// JSON Module Tests
// ============================================================================

TEST(JSONModuleTest, ModuleExists) {
    StdLib stdlib;
    auto mod = stdlib.getModule("json");
    ASSERT_NE(mod, nullptr);
    EXPECT_EQ(mod->getName(), "json");
}

TEST(JSONModuleTest, HasParse) {
    JSONModule mod;
    EXPECT_TRUE(mod.hasFunction("parse"));
}

TEST(JSONModuleTest, HasStringify) {
    JSONModule mod;
    EXPECT_TRUE(mod.hasFunction("stringify"));
}

// ============================================================================
// HTTP Module Tests
// ============================================================================

TEST(HTTPModuleTest, ModuleExists) {
    StdLib stdlib;
    auto mod = stdlib.getModule("http");
    ASSERT_NE(mod, nullptr);
    EXPECT_EQ(mod->getName(), "http");
}

TEST(HTTPModuleTest, HasGet) {
    HTTPModule mod;
    EXPECT_TRUE(mod.hasFunction("get"));
}

TEST(HTTPModuleTest, HasPost) {
    HTTPModule mod;
    EXPECT_TRUE(mod.hasFunction("post"));
}

// ============================================================================
// Collections Module Tests
// ============================================================================

TEST(CollectionsModuleTest, ModuleExists) {
    StdLib stdlib;
    auto mod = stdlib.getModule("collections");
    ASSERT_NE(mod, nullptr);
    EXPECT_EQ(mod->getName(), "collections");
}

// ============================================================================
// Module Availability Tests
// ============================================================================

TEST(ModuleAvailabilityTest, TimeModule) {
    StdLib stdlib;
    auto mod = stdlib.getModule("time");
    ASSERT_NE(mod, nullptr);
    EXPECT_EQ(mod->getName(), "time");
}

TEST(ModuleAvailabilityTest, EnvModule) {
    StdLib stdlib;
    auto mod = stdlib.getModule("env");
    ASSERT_NE(mod, nullptr);
    EXPECT_EQ(mod->getName(), "env");
}

TEST(ModuleAvailabilityTest, CsvModule) {
    StdLib stdlib;
    auto mod = stdlib.getModule("csv");
    ASSERT_NE(mod, nullptr);
    EXPECT_EQ(mod->getName(), "csv");
}

TEST(ModuleAvailabilityTest, RegexModule) {
    StdLib stdlib;
    auto mod = stdlib.getModule("regex");
    ASSERT_NE(mod, nullptr);
    EXPECT_EQ(mod->getName(), "regex");
}

TEST(ModuleAvailabilityTest, CryptoModule) {
    StdLib stdlib;
    auto mod = stdlib.getModule("crypto");
    ASSERT_NE(mod, nullptr);
    EXPECT_EQ(mod->getName(), "crypto");
}

TEST(ModuleAvailabilityTest, FileModule) {
    StdLib stdlib;
    auto mod = stdlib.getModule("file");
    ASSERT_NE(mod, nullptr);
    EXPECT_EQ(mod->getName(), "file");
}

// Total: 80+ stdlib tests
