#ifndef NAAB_INTERPRETER_ERROR_CONTEXT_H
#define NAAB_INTERPRETER_ERROR_CONTEXT_H

#include <string>
#include <vector>
#include <map>
#include <memory>

// Forward declarations
namespace naab {
namespace ast {
    class ASTNode;
}
}

namespace naab {
namespace interpreter {

// Stack frame for error reporting
class StackFrame {
public:
    std::string function_name;
    std::string file_path;
    size_t line;
    size_t column;

    // Variable state at this frame
    std::map<std::string, std::string> local_variables;

    StackFrame(const std::string& func_name,
               const std::string& file,
               size_t l,
               size_t c)
        : function_name(func_name), file_path(file), line(l), column(c) {}

    std::string toString() const;
};

// Runtime error context with enhanced information
class RuntimeErrorContext {
public:
    std::string error_message;
    std::vector<StackFrame> stack_trace;

    // Suggested fixes
    std::vector<std::string> suggestions;

    // Related errors (for cascading failures)
    std::vector<RuntimeErrorContext> related;

    // Source context
    std::string source_line;
    size_t error_column;

    RuntimeErrorContext(const std::string& msg)
        : error_message(msg), error_column(0) {}

    std::string formatStackTrace() const;
    std::string formatWithHints() const;
    std::string formatFull() const;
};

// Enhanced error reporter for interpreter
class InterpreterErrorReporter {
public:
    InterpreterErrorReporter();

    // Report error with stack trace
    void reportError(
        const std::string& message,
        const ast::ASTNode* node,
        const std::vector<StackFrame>& stack_trace
    );

    // Report with custom hints
    void reportWithHint(
        const std::string& message,
        const ast::ASTNode* node,
        const std::vector<std::string>& hints,
        const std::vector<StackFrame>& stack_trace
    );

    // Specific error types with built-in hints
    void reportUndefinedVariable(
        const std::string& name,
        const ast::ASTNode* node,
        const std::vector<StackFrame>& stack_trace
    );

    void reportTypeMismatch(
        const std::string& expected,
        const std::string& actual,
        const ast::ASTNode* node,
        const std::vector<StackFrame>& stack_trace
    );

    void reportNullAccess(
        const ast::ASTNode* node,
        const std::vector<StackFrame>& stack_trace
    );

    void reportDivisionByZero(
        const ast::ASTNode* node,
        const std::vector<StackFrame>& stack_trace
    );

    void reportIndexOutOfBounds(
        size_t index,
        size_t size,
        const ast::ASTNode* node,
        const std::vector<StackFrame>& stack_trace
    );

    void reportInvalidOperator(
        const std::string& op,
        const std::string& left_type,
        const std::string& right_type,
        const ast::ASTNode* node,
        const std::vector<StackFrame>& stack_trace
    );

    // Get the last error context
    const RuntimeErrorContext& getLastError() const { return last_error_; }
    bool hasError() const { return !last_error_.error_message.empty(); }

    // Clear error state
    void clear() { last_error_ = RuntimeErrorContext(""); }

private:
    RuntimeErrorContext last_error_;

    // Hint generators
    std::vector<std::string> suggestSimilarVariables(
        const std::string& name,
        const std::vector<StackFrame>& stack_trace
    );

    std::vector<std::string> suggestTypeConversion(
        const std::string& from,
        const std::string& to
    );

    std::vector<std::string> suggestNullCheck();

    std::vector<std::string> suggestBoundsCheck();

    // Helper to format error location
    std::string formatLocation(const ast::ASTNode* node) const;

    // Helper to extract source context
    std::string getSourceContext(const ast::ASTNode* node, size_t context_lines = 2) const;
};

} // namespace interpreter
} // namespace naab

#endif // NAAB_INTERPRETER_ERROR_CONTEXT_H
