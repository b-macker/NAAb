#include "naab/expression_detector.h"
#include <algorithm>
#include <cctype>
#include <string>

namespace naab {
namespace runtime {

std::string ExpressionDetector::trim(const std::string& str) {
    auto start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";

    auto end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

bool ExpressionDetector::isExpression(const std::string& code, const std::string& language) {
    if (language == "rust") return isRustExpression(code);
    if (language == "ruby") return isRubyExpression(code);
    if (language == "go") return isGoExpression(code);
    if (language == "csharp" || language == "cs") return isCSharpExpression(code);
    return false;
}

bool ExpressionDetector::isRustExpression(const std::string& code) {
    std::string trimmed = trim(code);

    // Not an expression if contains:
    // - fn keyword (function definition)
    // - use/mod/struct/impl keywords (declarations)
    // - ends with semicolon (statements end with ;, expressions don't)
    if (trimmed.find("fn ") != std::string::npos) return false;
    if (trimmed.find("use ") != std::string::npos) return false;
    if (trimmed.find("mod ") != std::string::npos) return false;
    if (trimmed.find("struct ") != std::string::npos) return false;
    if (trimmed.find("impl ") != std::string::npos) return false;
    if (trimmed.find("let ") == 0) return false;  // let statements aren't expressions

    // Rust expressions don't end with semicolon
    if (!trimmed.empty() && trimmed.back() == ';') return false;

    // If it's a single line and doesn't have keywords, likely an expression
    return true;
}

bool ExpressionDetector::isRubyExpression(const std::string& code) {
    std::string trimmed = trim(code);

    // Not an expression if contains:
    // - def keyword (function definition)
    // - class/module keywords
    // - puts/print (output statements that return nil)
    if (trimmed.find("def ") != std::string::npos) return false;
    if (trimmed.find("class ") != std::string::npos) return false;
    if (trimmed.find("module ") != std::string::npos) return false;
    if (trimmed.find("puts ") != std::string::npos) return false;
    if (trimmed.find("print ") != std::string::npos) return false;
    if (trimmed.find("p ") == 0) return false;  // p is debug print

    return true;
}

bool ExpressionDetector::isGoExpression(const std::string& code) {
    std::string trimmed = trim(code);

    // Go expressions are rare - most Go code needs package main
    // Not an expression if contains:
    // - package keyword
    // - func keyword
    // - import keyword
    // - fmt.Println (output statement)
    if (trimmed.find("package ") != std::string::npos) return false;
    if (trimmed.find("func ") != std::string::npos) return false;
    if (trimmed.find("import ") != std::string::npos) return false;
    if (trimmed.find("fmt.Println") != std::string::npos) return false;
    if (trimmed.find("fmt.Printf") != std::string::npos) return false;

    // Only allow very simple expressions (single line, basic operations)
    // Go's type system makes complex expressions difficult to wrap
    return (trimmed.find('\n') == std::string::npos);
}

bool ExpressionDetector::isCSharpExpression(const std::string& code) {
    std::string trimmed = trim(code);

    // Not an expression if contains:
    // - using keyword (imports)
    // - class/namespace keywords
    // - Console.WriteLine (output statement)
    if (trimmed.find("using ") != std::string::npos) return false;
    if (trimmed.find("class ") != std::string::npos) return false;
    if (trimmed.find("namespace ") != std::string::npos) return false;
    if (trimmed.find("Console.WriteLine") != std::string::npos) return false;
    if (trimmed.find("Console.Write") != std::string::npos) return false;

    return true;
}

} // namespace runtime
} // namespace naab
