// C++ block executor and the language registry.
//
// Folded in from three standalone programs in the default build target that
// stopped compiling at the NaabVal migration and that nothing built or ran
// (A58): tests/unit/test_cpp_executor.cpp, test_cpp_compilation_suite.cpp and
// test_language_registry.cpp. The first and third read their block from
// examples/test_cpp_block_add.cpp, which the 2026-02-20 repository cleanup
// (7e52ae7e) deleted; the block is written here instead. The compilation
// suite's library-detection checks are not carried over: they tested
// BlockEnricher, a tool deleted on purpose as stale (9efc86fe, 2026-02-21),
// whose detectLibraries() is now a stub returning nothing. Compiling needs a
// C++ compiler on PATH: without one the compiling cases SKIP, and say why.

#include "naab/cpp_executor.h"
#include "naab/interpreter.h"
#include "naab/js_executor_adapter.h"
#include "naab/language_registry.h"
#include "naab/sandbox.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

using naab::interpreter::NaabVal;
using naab::runtime::CppExecutor;

namespace {

bool haveCompiler() {
    return std::system("command -v g++ >/dev/null 2>&1 || command -v c++ >/dev/null 2>&1") == 0;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

naab::security::SandboxConfig level(naab::security::PermissionLevel l) {
    return naab::security::SandboxConfig::fromPermissionLevel(l);
}

const char* kAddMultiply = R"(
    extern "C" {
        int add(int a, int b) { return a + b; }
        int multiply(int a, int b) { return a * b; }
    }
)";

}  // namespace

TEST(CppBlockTest, CompilesAndCallsIntFunctions) {
    if (!haveCompiler()) GTEST_SKIP() << "no C++ compiler on PATH";
    CppExecutor cpp;
    ASSERT_TRUE(cpp.compileBlock("UNIT-ADD-MUL", kAddMultiply, "add"));
    EXPECT_TRUE(cpp.isCompiled("UNIT-ADD-MUL"));
    EXPECT_EQ(cpp.callFunction("UNIT-ADD-MUL", "add", {NaabVal::makeInt(5), NaabVal::makeInt(3)}).toInt(), 8);
    EXPECT_EQ(cpp.callFunction("UNIT-ADD-MUL", "add", {NaabVal::makeInt(10), NaabVal::makeInt(20)}).toInt(), 30);
    EXPECT_EQ(cpp.callFunction("UNIT-ADD-MUL", "multiply", {NaabVal::makeInt(7), NaabVal::makeInt(6)}).toInt(), 42);
}

TEST(CppBlockTest, StandardLibraryBlockCompiles) {
    if (!haveCompiler()) GTEST_SKIP() << "no C++ compiler on PATH";
    const std::string code = R"(
        #include <string>
        #include <vector>
        #include <cmath>
        extern "C" {
            double compute_magnitude() { return std::sqrt(3.0 * 3.0 + 4.0 * 4.0); }
        }
    )";
    CppExecutor cpp;
    EXPECT_TRUE(cpp.compileBlock("UNIT-STDLIB", code, "compute_magnitude"));
}

TEST(LanguageRegistryTest, RegistersAndServesAnExecutor) {
    naab::runtime::LanguageRegistry registry;
    EXPECT_FALSE(registry.isSupported("javascript"));
    registry.registerExecutor("javascript", std::make_unique<naab::runtime::JsExecutorAdapter>());
    EXPECT_TRUE(registry.isSupported("javascript"));
    EXPECT_FALSE(registry.isSupported("no-such-language"));
    EXPECT_TRUE(contains(registry.supportedLanguages(), "javascript"));

    // getExecutor() is the one polyglot gate: a policy that refuses blocks
    // refuses here, and one that allows them is served. Each arm installs its
    // own sandbox, so neither depends on what an earlier test left as the
    // process default.
    {
        naab::security::ScopedSandbox restricted(level(naab::security::PermissionLevel::RESTRICTED));
        EXPECT_THROW(registry.getExecutor("javascript"), std::exception);
    }
    {
        naab::security::ScopedSandbox standard(level(naab::security::PermissionLevel::STANDARD));
        auto* js = registry.getExecutor("javascript");
        ASSERT_NE(js, nullptr);
        EXPECT_EQ(js->executeWithReturn("1 + 1").toInt(), 2);
    }

    registry.unregisterExecutor("javascript");
    EXPECT_FALSE(registry.isSupported("javascript"));
}
