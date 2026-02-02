#include "error_context.h"
#include "../include/naab/ast.h"
#include <fmt/core.h>
#include <sstream>
#include <algorithm>

namespace naab {
namespace interpreter {

// ============================================================================
// StackFrame Implementation
// ============================================================================

std::string StackFrame::toString() const {
    std::ostringstream oss;
    oss << "    at " << function_name;

    if (!file_path.empty()) {
        oss << " (" << file_path << ":" << line << ":" << column << ")";
    }

    return oss.str();
}

// ============================================================================
// RuntimeErrorContext Implementation
// ============================================================================

std::string RuntimeErrorContext::formatStackTrace() const {
    std::ostringstream oss;

    if (stack_trace.empty()) {
        return "";
    }

    oss << "\nStack trace:\n";
    for (const auto& frame : stack_trace) {
        oss << frame.toString() << "\n";
    }

    return oss.str();
}

std::string RuntimeErrorContext::formatWithHints() const {
    std::ostringstream oss;

    // Error message
    oss << "Runtime Error: " << error_message << "\n";

    // Stack trace
    oss << formatStackTrace();

    // Suggestions
    if (!suggestions.empty()) {
        oss << "\nHint: " << suggestions[0] << "\n";

        for (size_t i = 1; i < suggestions.size(); ++i) {
            oss << "      " << suggestions[i] << "\n";
        }
    }

    return oss.str();
}

std::string RuntimeErrorContext::formatFull() const {
    std::ostringstream oss;

    oss << formatWithHints();

    // Source context
    if (!source_line.empty()) {
        oss << "\nContext:\n";
        oss << "    " << source_line << "\n";

        if (error_column > 0) {
            oss << "    " << std::string(error_column, ' ') << "^\n";
        }
    }

    // Local variables (from top stack frame)
    if (!stack_trace.empty() && !stack_trace[0].local_variables.empty()) {
        oss << "\nLocal variables:\n";
        for (const auto& [name, value] : stack_trace[0].local_variables) {
            oss << "    " << name << " = " << value << "\n";
        }
    }

    return oss.str();
}

// ============================================================================
// InterpreterErrorReporter Implementation
// ============================================================================

InterpreterErrorReporter::InterpreterErrorReporter()
    : last_error_("") {}

void InterpreterErrorReporter::reportError(
    const std::string& message,
    const ast::ASTNode* node,
    const std::vector<StackFrame>& stack_trace
) {
    RuntimeErrorContext ctx(message);
    ctx.stack_trace = stack_trace;

    if (node) {
        ctx.source_line = getSourceContext(node);
        ctx.error_column = node->getLocation().column;
    }

    last_error_ = ctx;

    // Print immediately (can be changed to just store)
    fmt::print("{}\n", ctx.formatWithHints());
}

void InterpreterErrorReporter::reportWithHint(
    const std::string& message,
    const ast::ASTNode* node,
    const std::vector<std::string>& hints,
    const std::vector<StackFrame>& stack_trace
) {
    RuntimeErrorContext ctx(message);
    ctx.stack_trace = stack_trace;
    ctx.suggestions = hints;

    if (node) {
        ctx.source_line = getSourceContext(node);
        ctx.error_column = node->getLocation().column;
    }

    last_error_ = ctx;

    fmt::print("{}\n", ctx.formatWithHints());
}

void InterpreterErrorReporter::reportUndefinedVariable(
    const std::string& name,
    const ast::ASTNode* node,
    const std::vector<StackFrame>& stack_trace
) {
    std::string message = fmt::format("Undefined variable '{}'", name);

    auto hints = suggestSimilarVariables(name, stack_trace);
    if (hints.empty()) {
        hints.push_back(fmt::format("Variable '{}' is not defined.", name));
        hints.push_back("Did you forget to declare it with 'let'?");
    }

    reportWithHint(message, node, hints, stack_trace);
}

void InterpreterErrorReporter::reportTypeMismatch(
    const std::string& expected,
    const std::string& actual,
    const ast::ASTNode* node,
    const std::vector<StackFrame>& stack_trace
) {
    std::string message = fmt::format(
        "Type mismatch: expected '{}', got '{}'",
        expected,
        actual
    );

    auto hints = suggestTypeConversion(actual, expected);

    reportWithHint(message, node, hints, stack_trace);
}

void InterpreterErrorReporter::reportNullAccess(
    const ast::ASTNode* node,
    const std::vector<StackFrame>& stack_trace
) {
    std::string message = "Attempted to access member of null value";
    auto hints = suggestNullCheck();

    reportWithHint(message, node, hints, stack_trace);
}

void InterpreterErrorReporter::reportDivisionByZero(
    const ast::ASTNode* node,
    const std::vector<StackFrame>& stack_trace
) {
    std::string message = "Division by zero";

    std::vector<std::string> hints = {
        "Cannot divide by zero.",
        "",
        "Add a check before division:",
        "    if divisor != 0 {",
        "        result = numerator / divisor",
        "    }",
    };

    reportWithHint(message, node, hints, stack_trace);
}

void InterpreterErrorReporter::reportIndexOutOfBounds(
    size_t index,
    size_t size,
    const ast::ASTNode* node,
    const std::vector<StackFrame>& stack_trace
) {
    std::string message = fmt::format(
        "Index out of bounds: index {} >= size {}",
        index,
        size
    );

    auto hints = suggestBoundsCheck();
    hints.insert(hints.begin(), fmt::format(
        "Valid indices are 0 to {} (got {}).",
        size - 1,
        index
    ));

    reportWithHint(message, node, hints, stack_trace);
}

void InterpreterErrorReporter::reportInvalidOperator(
    const std::string& op,
    const std::string& left_type,
    const std::string& right_type,
    const ast::ASTNode* node,
    const std::vector<StackFrame>& stack_trace
) {
    std::string message = fmt::format(
        "Invalid operator '{}' for types '{}' and '{}'",
        op,
        left_type,
        right_type
    );

    std::vector<std::string> hints = {
        fmt::format("Operator '{}' is not defined for these types.", op),
        "",
        "Possible solutions:",
        "    - Convert values to compatible types",
        "    - Use a different operator",
    };

    reportWithHint(message, node, hints, stack_trace);
}

// ============================================================================
// Hint Generators
// ============================================================================

std::vector<std::string> InterpreterErrorReporter::suggestSimilarVariables(
    const std::string& name,
    const std::vector<StackFrame>& stack_trace
) {
    std::vector<std::string> hints;

    // Simple suggestion: check for common typos
    // In a real implementation, this would use Levenshtein distance
    // to find similar variable names from the stack frames

    if (!stack_trace.empty() && !stack_trace[0].local_variables.empty()) {
        hints.push_back(fmt::format("Variable '{}' is not defined. Did you mean one of these?", name));

        // Find similar names (simple prefix match for now)
        for (const auto& [var_name, value] : stack_trace[0].local_variables) {
            if (var_name.find(name.substr(0, std::min(size_t(3), name.length()))) == 0) {
                hints.push_back(fmt::format("    - {} (value: {})", var_name, value));
            }
        }
    }

    return hints;
}

std::vector<std::string> InterpreterErrorReporter::suggestTypeConversion(
    const std::string& from,
    const std::string& to
) {
    std::vector<std::string> hints;

    hints.push_back(fmt::format("Cannot implicitly convert '{}' to '{}'.", from, to));
    hints.push_back("");

    // Common conversions
    if (from == "string" && to == "int") {
        hints.push_back("Try converting the string to an integer:");
        hints.push_back("    let num = int(str_value)");
    } else if (from == "int" && to == "string") {
        hints.push_back("Try converting the integer to a string:");
        hints.push_back("    let str = string(int_value)");
    } else if (from == "null" && to != "null") {
        hints.push_back("This value might be null. Add a null check:");
        hints.push_back("    if value != null {");
        hints.push_back("        // use value");
        hints.push_back("    }");
    } else {
        hints.push_back("Ensure the value is of the correct type.");
    }

    return hints;
}

std::vector<std::string> InterpreterErrorReporter::suggestNullCheck() {
    return {
        "This value is null. Add a null check before accessing members.",
        "",
        "Example:",
        "    if obj != null {",
        "        value = obj.field",
        "    }",
        "",
        "Or use optional chaining (if available):",
        "    value = obj?.field",
    };
}

std::vector<std::string> InterpreterErrorReporter::suggestBoundsCheck() {
    return {
        "",
        "Add a bounds check before accessing:",
        "    if index < array.length() {",
        "        value = array[index]",
        "    }",
    };
}

// ============================================================================
// Helper Functions
// ============================================================================

std::string InterpreterErrorReporter::formatLocation(const ast::ASTNode* node) const {
    if (!node) {
        return "";
    }

    auto loc = node->getLocation();
    return fmt::format("{}:{}:{}", loc.filename, loc.line, loc.column);
}

std::string InterpreterErrorReporter::getSourceContext(
    const ast::ASTNode* node,
    size_t context_lines
) const {
    (void)context_lines;  // Unused - full implementation would use this

    if (!node) {
        return "";
    }

    // In a full implementation, this would read the source file
    // and extract context_lines before and after the error line
    // For now, return a placeholder
    return fmt::format("<source line {}>", node->getLocation().line);
}

} // namespace interpreter
} // namespace naab
