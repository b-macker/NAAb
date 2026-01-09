#ifndef NAAB_BLOCK_TESTER_H
#define NAAB_BLOCK_TESTER_H

// NAAb Block Testing Framework
// Tests individual blocks in isolation before using them

#include <string>
#include <vector>
#include <memory>

namespace naab {
namespace interpreter {
    class Value;  // Forward declaration
}

namespace testing {

// Assertion types
enum class AssertionType {
    EQUALS,          // value == expected
    NOT_EQUALS,      // value != expected
    GREATER_THAN,    // value > expected
    LESS_THAN,       // value < expected
    CONTAINS,        // string contains substring
    TYPE_IS          // check value type
};

// Single assertion
struct Assertion {
    AssertionType type;
    std::string value_expr;  // Expression to evaluate (e.g., "result.status")
    std::string expected;    // Expected value
};

// Single test case
struct BlockTest {
    std::string name;
    std::string code;                    // Code to execute
    std::vector<Assertion> assertions;   // Assertions to check
};

// Test definition for a block
struct BlockTestDefinition {
    std::string block_id;
    std::string language;
    std::string setup_code;              // Optional setup code
    std::vector<BlockTest> tests;
};

// Test result for a single test
struct TestResult {
    std::string test_name;
    bool passed;
    std::string error_message;
    double execution_time_ms;
};

// Overall test results
struct TestResults {
    std::string block_id;
    int total;
    int passed;
    int failed;
    std::vector<TestResult> results;

    bool allPassed() const { return failed == 0; }
};

// Block Testing Framework
class BlockTester {
public:
    BlockTester();
    ~BlockTester() = default;

    // Load test definition from JSON file
    bool loadTestDefinition(const std::string& test_file_path);

    // Run all tests for the loaded block
    TestResults runTests();

    // Run tests for a specific block ID (loads from standard location)
    TestResults runTestsForBlock(const std::string& block_id);

    // Get test definition directory
    static std::string getTestDefinitionDir();

private:
    BlockTestDefinition definition_;

    // Run a single test case
    TestResult runSingleTest(const BlockTest& test);

    // Check a single assertion
    bool checkAssertion(const Assertion& assertion,
                        const std::shared_ptr<interpreter::Value>& result,
                        std::string& error_message);

    // Parse JSON test definition
    bool parseTestDefinition(const std::string& json_content);
};

} // namespace testing
} // namespace naab

#endif // NAAB_BLOCK_TESTER_H
