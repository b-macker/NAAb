// Stack Tracer Unit Tests (Phase 4.2)

#include <gtest/gtest.h>
#include "naab/stack_tracer.h"

using namespace naab::error;

class StackTracerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clear stack before each test
        StackTracer::clear();
    }

    void TearDown() override {
        // Clean up after test
        StackTracer::clear();
    }
};

// ============================================================================
// Basic Stack Operations
// ============================================================================

TEST_F(StackTracerTest, InitiallyEmpty) {
    EXPECT_EQ(StackTracer::depth(), 0);
    auto trace = StackTracer::getTrace();
    EXPECT_TRUE(trace.empty());
}

TEST_F(StackTracerTest, PushFrame) {
    StackFrame frame("naab", "test_function", "test.naab", 10);
    StackTracer::pushFrame(frame);

    EXPECT_EQ(StackTracer::depth(), 1);
    auto trace = StackTracer::getTrace();
    ASSERT_EQ(trace.size(), 1);
    EXPECT_EQ(trace[0].function_name, "test_function");
    EXPECT_EQ(trace[0].language, "naab");
}

TEST_F(StackTracerTest, PopFrame) {
    StackFrame frame("naab", "test_function", "test.naab", 10);
    StackTracer::pushFrame(frame);
    StackTracer::popFrame();

    EXPECT_EQ(StackTracer::depth(), 0);
}

TEST_F(StackTracerTest, MultipleFrames) {
    StackTracer::pushFrame(StackFrame("naab", "main", "main.naab", 1));
    StackTracer::pushFrame(StackFrame("python", "helper", "util.py", 42));
    StackTracer::pushFrame(StackFrame("javascript", "process", "lib.js", 100));

    EXPECT_EQ(StackTracer::depth(), 3);

    auto trace = StackTracer::getTrace();
    ASSERT_EQ(trace.size(), 3);
    EXPECT_EQ(trace[0].function_name, "main");
    EXPECT_EQ(trace[1].function_name, "helper");
    EXPECT_EQ(trace[2].function_name, "process");
}

TEST_F(StackTracerTest, Clear) {
    StackTracer::pushFrame(StackFrame("naab", "func1", "test.naab", 1));
    StackTracer::pushFrame(StackFrame("naab", "func2", "test.naab", 2));

    StackTracer::clear();

    EXPECT_EQ(StackTracer::depth(), 0);
}

// ============================================================================
// Scoped Stack Frame RAII
// ============================================================================

TEST_F(StackTracerTest, ScopedFrameRaii) {
    {
        ScopedStackFrame frame("naab", "scoped_test", "test.naab", 5);
        EXPECT_EQ(StackTracer::depth(), 1);
    }
    // Frame should auto-pop on scope exit
    EXPECT_EQ(StackTracer::depth(), 0);
}

TEST_F(StackTracerTest, ScopedFrameNestedScopes) {
    {
        ScopedStackFrame frame1("naab", "outer", "test.naab", 1);
        EXPECT_EQ(StackTracer::depth(), 1);

        {
            ScopedStackFrame frame2("python", "inner", "util.py", 10);
            EXPECT_EQ(StackTracer::depth(), 2);

            auto trace = StackTracer::getTrace();
            EXPECT_EQ(trace[0].function_name, "outer");
            EXPECT_EQ(trace[1].function_name, "inner");
        }

        // Inner frame popped
        EXPECT_EQ(StackTracer::depth(), 1);
    }

    // All frames popped
    EXPECT_EQ(StackTracer::depth(), 0);
}

TEST_F(StackTracerTest, ScopedFrameWithException) {
    try {
        ScopedStackFrame frame("naab", "throwing_func", "test.naab", 10);
        EXPECT_EQ(StackTracer::depth(), 1);
        throw std::runtime_error("test exception");
    } catch (...) {
        // Frame should still be popped
    }

    EXPECT_EQ(StackTracer::depth(), 0);
}

// ============================================================================
// Stack Frame Formatting
// ============================================================================

TEST_F(StackTracerTest, FormatSingleFrame) {
    StackFrame frame("naab", "my_function", "main.naab", 42);
    std::string formatted = frame.toString();

    EXPECT_NE(formatted.find("at my_function"), std::string::npos);
    EXPECT_NE(formatted.find("naab:main.naab:42"), std::string::npos);
}

TEST_F(StackTracerTest, FormatFrameWithoutFile) {
    StackFrame frame("cpp", "native_block");
    std::string formatted = frame.toString();

    EXPECT_NE(formatted.find("at native_block"), std::string::npos);
    EXPECT_NE(formatted.find("cpp:<native>"), std::string::npos);
}

TEST_F(StackTracerTest, FormatFullTrace) {
    StackTracer::pushFrame(StackFrame("naab", "main", "main.naab", 1));
    StackTracer::pushFrame(StackFrame("python", "process", "util.py", 50));
    StackTracer::pushFrame(StackFrame("javascript", "transform", "lib.js", 120));

    std::string formatted = StackTracer::formatTrace();

    // Should contain all function names
    EXPECT_NE(formatted.find("main"), std::string::npos);
    EXPECT_NE(formatted.find("process"), std::string::npos);
    EXPECT_NE(formatted.find("transform"), std::string::npos);

    // Should have header
    EXPECT_NE(formatted.find("Stack trace"), std::string::npos);
}

// ============================================================================
// Cross-Language Scenarios
// ============================================================================

TEST_F(StackTracerTest, CrossLanguageStack) {
    // Simulate: NAAb calls Python, which calls JavaScript, which calls Rust
    StackTracer::pushFrame(StackFrame("naab", "orchestrator", "main.naab", 10));
    StackTracer::pushFrame(StackFrame("python", "data_processor", "processor.py", 25));
    StackTracer::pushFrame(StackFrame("javascript", "validator", "validate.js", 78));
    StackTracer::pushFrame(StackFrame("rust", "compute_intensive", "compute.rs", 42));

    auto trace = StackTracer::getTrace();
    ASSERT_EQ(trace.size(), 4);

    EXPECT_EQ(trace[0].language, "naab");
    EXPECT_EQ(trace[1].language, "python");
    EXPECT_EQ(trace[2].language, "javascript");
    EXPECT_EQ(trace[3].language, "rust");
}
