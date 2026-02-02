// Error Categories Unit Tests (Phase 4.1.3 - Task 4.1.20)

#include <gtest/gtest.h>
#include "naab/error_categories.h"

using namespace naab::error;

// ============================================================================
// Category Identification Tests
// ============================================================================

TEST(ErrorCategoriesTest, GetErrorCodePrefix) {
    EXPECT_EQ(getErrorCodePrefix(ErrorCategory::TypeError), "E0");
    EXPECT_EQ(getErrorCodePrefix(ErrorCategory::RuntimeError), "E1");
    EXPECT_EQ(getErrorCodePrefix(ErrorCategory::ImportError), "E2");
    EXPECT_EQ(getErrorCodePrefix(ErrorCategory::SyntaxError), "E3");
    EXPECT_EQ(getErrorCodePrefix(ErrorCategory::NameError), "E4");
    EXPECT_EQ(getErrorCodePrefix(ErrorCategory::ValueError), "E5");
}

TEST(ErrorCategoriesTest, GetCategoryName) {
    EXPECT_EQ(getCategoryName(ErrorCategory::TypeError), "TypeError");
    EXPECT_EQ(getCategoryName(ErrorCategory::RuntimeError), "RuntimeError");
    EXPECT_EQ(getCategoryName(ErrorCategory::ImportError), "ImportError");
    EXPECT_EQ(getCategoryName(ErrorCategory::SyntaxError), "SyntaxError");
    EXPECT_EQ(getCategoryName(ErrorCategory::NameError), "NameError");
    EXPECT_EQ(getCategoryName(ErrorCategory::ValueError), "ValueError");
}

// ============================================================================
// TypeError Message Tests (Task 4.1.16)
// ============================================================================

TEST(ErrorCategoriesTest, FormatTypeError) {
    std::string msg = formatTypeError("int", "string");
    EXPECT_EQ(msg, "Expected type 'int' but got 'string'");
}

TEST(ErrorCategoriesTest, FormatConversionError) {
    std::string msg = formatConversionError("string", "int");
    EXPECT_EQ(msg, "Cannot convert type 'string' to 'int'");
}

TEST(ErrorCategoriesTest, FormatOperatorTypeError) {
    std::string msg = formatOperatorTypeError("+", "string", "int");
    EXPECT_EQ(msg, "Type mismatch in operator '+': cannot operate on 'string' and 'int'");
}

// ============================================================================
// RuntimeError Message Tests (Task 4.1.17)
// ============================================================================

TEST(ErrorCategoriesTest, FormatDivisionByZero) {
    std::string msg = formatDivisionByZero();
    EXPECT_EQ(msg, "Division by zero");
}

TEST(ErrorCategoriesTest, FormatNullPointerAccess) {
    std::string msg1 = formatNullPointerAccess();
    EXPECT_EQ(msg1, "Null pointer access");

    std::string msg2 = formatNullPointerAccess("method call");
    EXPECT_EQ(msg2, "Null pointer access in method call");
}

TEST(ErrorCategoriesTest, FormatIndexOutOfBounds) {
    std::string msg = formatIndexOutOfBounds(10, 5);
    EXPECT_EQ(msg, "Index 10 out of bounds for container of size 5");
}

// ============================================================================
// ImportError Message Tests (Task 4.1.18)
// ============================================================================

TEST(ErrorCategoriesTest, FormatModuleNotFound) {
    std::string msg = formatModuleNotFound("math");
    EXPECT_EQ(msg, "Module 'math' not found");
}

TEST(ErrorCategoriesTest, FormatCircularImport) {
    std::string msg = formatCircularImport("module_a");
    EXPECT_EQ(msg, "Circular import detected: 'module_a'");
}

TEST(ErrorCategoriesTest, FormatInvalidImportPath) {
    std::string msg = formatInvalidImportPath("/invalid/path");
    EXPECT_EQ(msg, "Invalid import path: '/invalid/path'");
}

// ============================================================================
// Error Code Constants Tests (Task 4.1.19)
// ============================================================================

TEST(ErrorCategoriesTest, TypeErrorCodes) {
    EXPECT_EQ(E001_TYPE_MISMATCH, 1);
    EXPECT_EQ(E002_CANNOT_CONVERT, 2);
    EXPECT_EQ(E003_OPERATOR_TYPE_ERROR, 3);
}

TEST(ErrorCategoriesTest, RuntimeErrorCodes) {
    EXPECT_EQ(E100_DIVISION_BY_ZERO, 100);
    EXPECT_EQ(E101_NULL_POINTER, 101);
    EXPECT_EQ(E102_INDEX_OUT_OF_BOUNDS, 102);
}

TEST(ErrorCategoriesTest, ImportErrorCodes) {
    EXPECT_EQ(E200_MODULE_NOT_FOUND, 200);
    EXPECT_EQ(E201_CIRCULAR_IMPORT, 201);
    EXPECT_EQ(E202_INVALID_IMPORT_PATH, 202);
}

TEST(ErrorCategoriesTest, NameErrorCodes) {
    EXPECT_EQ(E400_UNDEFINED_VARIABLE, 400);
    EXPECT_EQ(E401_UNDEFINED_FUNCTION, 401);
    EXPECT_EQ(E402_UNDEFINED_MODULE, 402);
}

TEST(ErrorCategoriesTest, ValueErrorCodes) {
    EXPECT_EQ(E500_INVALID_VALUE, 500);
    EXPECT_EQ(E501_OUT_OF_RANGE, 501);
    EXPECT_EQ(E502_INVALID_ARGUMENT, 502);
}
