// One call into each of ten stdlib modules, made directly on the module.
//
// Was tests/unit/test_stdlib_modules.cpp, a standalone program in the default
// build target that stopped compiling at the NaabVal migration and that
// nothing built or ran (A58). Two of its twelve checks could not fail: env.has
// counted a miss as a pass ("may be platform-specific") and json.parse_object
// asserted nothing about the result. Both now check what they name, with a
// variable this test sets and a value it reads back. math.abs_fn is math.abs
// since the module's rename; file.exists reads a file this test makes rather
// than "/".

#include "naab/stdlib.h"
#include "naab/interpreter.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using naab::interpreter::NaabVal;

namespace {

NaabVal callFn(naab::stdlib::StdLib& lib, const std::string& module, const std::string& fn,
               std::vector<NaabVal> args) {
    auto m = lib.getModule(module);
    if (!m) throw std::runtime_error("no module " + module);
    return m->call(fn, args);
}

}  // namespace

TEST(StdlibModulesTest, StringLength) {
    naab::stdlib::StdLib lib;
    EXPECT_EQ(callFn(lib, "string", "length", {NaabVal::makeString("hello")}).toInt(), 5);
}

TEST(StdlibModulesTest, ArrayLength) {
    naab::stdlib::StdLib lib;
    NaabVal arr = NaabVal::makeList({NaabVal::makeInt(1), NaabVal::makeInt(2), NaabVal::makeInt(3)});
    EXPECT_EQ(callFn(lib, "array", "length", {arr}).toInt(), 3);
}

TEST(StdlibModulesTest, MathAbs) {
    naab::stdlib::StdLib lib;
    EXPECT_EQ(callFn(lib, "math", "abs", {NaabVal::makeInt(-42)}).toInt(), 42);
}

TEST(StdlibModulesTest, TimeNowIsPositive) {
    naab::stdlib::StdLib lib;
    EXPECT_GT(callFn(lib, "time", "now", {}).toInt(), 0);
}

TEST(StdlibModulesTest, EnvHasNamesPresentAndAbsentVariables) {
    naab::stdlib::StdLib lib;
#ifdef _WIN32
    _putenv_s("NAAB_A58_PRESENT", "1");
#else
    setenv("NAAB_A58_PRESENT", "1", 1);
#endif
    EXPECT_TRUE(callFn(lib, "env", "has", {NaabVal::makeString("NAAB_A58_PRESENT")}).toBool());
    EXPECT_FALSE(callFn(lib, "env", "has", {NaabVal::makeString("NAAB_A58_NEVER_SET")}).toBool());
}

TEST(StdlibModulesTest, CsvFormatRow) {
    naab::stdlib::StdLib lib;
    NaabVal row = NaabVal::makeList({NaabVal::makeString("a"), NaabVal::makeString("b"), NaabVal::makeString("c")});
    EXPECT_EQ(callFn(lib, "csv", "format_row", {row}).toString(), "a,b,c");
}

TEST(StdlibModulesTest, RegexIsValid) {
    naab::stdlib::StdLib lib;
    EXPECT_TRUE(callFn(lib, "regex", "is_valid", {NaabVal::makeString("[a-z]+")}).toBool());
    EXPECT_FALSE(callFn(lib, "regex", "is_valid", {NaabVal::makeString("[a-z")}).toBool());
}

TEST(StdlibModulesTest, CryptoBase64Encode) {
    naab::stdlib::StdLib lib;
    EXPECT_EQ(callFn(lib, "crypto", "base64_encode", {NaabVal::makeString("hello")}).toString(), "aGVsbG8=");
}

TEST(StdlibModulesTest, FileExists) {
    naab::stdlib::StdLib lib;
    const std::string name = "naab_a58_file_exists_probe.txt";
    { std::ofstream(name) << "x"; }
    EXPECT_TRUE(callFn(lib, "file", "exists", {NaabVal::makeString(name)}).toBool());
    std::remove(name.c_str());
    EXPECT_FALSE(callFn(lib, "file", "exists", {NaabVal::makeString(name)}).toBool());
}

TEST(StdlibModulesTest, JsonParseObjectAndIsValid) {
    naab::stdlib::StdLib lib;
    NaabVal obj = callFn(lib, "json", "parse_object", {NaabVal::makeString("{\"key\":\"value\"}")});
    ASSERT_TRUE(obj.isDict());
    ASSERT_EQ(obj.asDictConst().count("key"), 1u);
    EXPECT_EQ(obj.asDictConst().at("key").toString(), "value");
    EXPECT_TRUE(callFn(lib, "json", "is_valid", {NaabVal::makeString("{\"test\":true}")}).toBool());
    EXPECT_FALSE(callFn(lib, "json", "is_valid", {NaabVal::makeString("{\"test\":")}).toBool());
}

TEST(StdlibModulesTest, HttpHasGet) {
    naab::stdlib::StdLib lib;
    auto http = lib.getModule("http");
    ASSERT_NE(http, nullptr);
    EXPECT_TRUE(http->hasFunction("get"));
}
