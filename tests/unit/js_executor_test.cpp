// JavaScript executor (QuickJS) and its registry adapter.
//
// Folded in from tests/unit/test_js_executor.cpp and test_block_tester.cpp,
// standalone programs in the default build target that stopped compiling at
// the NaabVal migration and that nothing built or ran (A58). The second never
// called BlockTester; it ran these same calls through the registry. The
// adapter cases are A61: a QuickJS runtime belongs to the thread that made it.

#include "naab/js_executor.h"
#include "naab/js_executor_adapter.h"
#include "naab/interpreter.h"
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

using naab::interpreter::NaabVal;
using naab::runtime::JsExecutor;
using naab::runtime::JsExecutorAdapter;

namespace {

const char* kLibrary = R"(
    function add(a, b) { return a + b; }
    function multiply(a, b) { return a * b; }
    function greet(name) { return "Hello, " + name + "!"; }
)";

}  // namespace

TEST(JsExecutorTest, CallsFunctionsItDefined) {
    JsExecutor js;
    ASSERT_TRUE(js.isInitialized());
    // INLINE_CODE (the default) runs the code inside a function so blocks do
    // not share variables; a library of functions is loaded in BLOCK_LIBRARY
    // mode (52637c19), which the original program predated.
    ASSERT_TRUE(js.execute(kLibrary, naab::runtime::JsExecutionMode::BLOCK_LIBRARY));

    NaabVal sum = js.callFunction("add", {NaabVal::makeInt(5), NaabVal::makeInt(3)});
    EXPECT_TRUE(sum.isInt());
    EXPECT_EQ(sum.toInt(), 8);
    EXPECT_EQ(js.callFunction("multiply", {NaabVal::makeInt(7), NaabVal::makeInt(6)}).toInt(), 42);
    EXPECT_EQ(js.callFunction("greet", {NaabVal::makeString("NAAb")}).toString(), "Hello, NAAb!");
}

TEST(JsExecutorTest, EvaluatesAnExpression) {
    JsExecutor js;
    EXPECT_EQ(js.evaluate("2 + 2 * 3").toInt(), 8);
}

TEST(JsExecutorTest, ReportsAnErrorItCanName) {
    JsExecutor js;
    try {
        js.evaluate("undefinedName + 1");
        FAIL() << "a ReferenceError should throw";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("is not defined"), std::string::npos) << e.what();
    }
}

// A61. The registry holds one adapter, made on the main thread, and a VM
// async fn calls it from its own thread. On that thread the adapter's own
// runtime failed every evaluation ("Unknown error"); the call must succeed.
TEST(JsExecutorAdapterTest, EvaluatesOnAnotherThread) {
    JsExecutorAdapter adapter;
    EXPECT_EQ(adapter.executeWithReturn("1 + 2").toInt(), 3);  // the owner thread

    NaabVal from_thread;
    std::string error;
    std::thread t([&] {
        try {
            from_thread = adapter.executeWithReturn("40 + 2");
        } catch (const std::exception& e) {
            error = e.what();
        }
    });
    t.join();
    EXPECT_EQ(error, "");
    EXPECT_EQ(from_thread.toInt(), 42);
}

// A call from another thread gets a runtime of its own: state made on the
// owner thread is not there, and the owner's state is untouched by it.
TEST(JsExecutorAdapterTest, AnotherThreadDoesNotShareTheOwnersRuntime) {
    JsExecutorAdapter adapter;
    ASSERT_EQ(adapter.executeWithReturn("globalThis.ownerMark = 7").toInt(), 7);

    std::string seen, error;
    std::thread t([&] {
        try {
            seen = adapter.executeWithReturn("typeof globalThis.ownerMark").toString();
        } catch (const std::exception& e) {
            error = e.what();
        }
    });
    t.join();
    EXPECT_EQ(error, "");
    EXPECT_EQ(seen, "undefined");
    EXPECT_EQ(adapter.executeWithReturn("globalThis.ownerMark").toInt(), 7);
}

// The last call's error text is kept per thread: a failing call on one thread
// is not reported as another thread's result.
TEST(JsExecutorAdapterTest, LastErrorIsPerThread) {
    JsExecutorAdapter adapter;
    EXPECT_THROW(adapter.executeWithReturn("undefinedName + 1"), std::exception);
    EXPECT_EQ(adapter.getLastExitCode(), 1);
    EXPECT_NE(adapter.getCapturedOutput(), "");

    int other_exit = -1;
    std::string other_output = "unset";
    std::thread t([&] {
        other_exit = adapter.getLastExitCode();
        other_output = adapter.getCapturedOutput();
    });
    t.join();
    EXPECT_EQ(other_exit, 0);
    EXPECT_EQ(other_output, "");
}
