#ifndef NAAB_INTERPRETER_H
#define NAAB_INTERPRETER_H

// NAAb Block Assembly Language - Interpreter
// Direct AST execution with visitor pattern

#include "naab/ast.h"
#include "naab/block_loader.h"
#include "naab/stdlib.h"
#include "naab/cpp_executor.h"
#include "naab/language_registry.h"
#include "naab/debugger.h"
#include "naab/module_resolver.h"  // Phase 3.1
#include "naab/environment.h" // GEMINI FIX: Added include for Environment
#include "naab/value.h" // GEMINI FIX: Added include for Value
#include <Python.h>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>

namespace naab {

namespace interpreter {

// Forward declarations
// class Value; // GEMINI FIX: Removed forward declaration as full header is included
// class Environment; // GEMINI FIX: Removed forward declaration as full header is included

#include "naab/block_value.h"

#include "naab/function_value.h"

#include "naab/python_object_value.h"

// ============================================================================
// Error Handling - Phase 4.1
// ============================================================================

// Error types for classification
enum class ErrorType {
    GENERIC,          // Generic error
    TYPE_ERROR,       // Type mismatch error
    RUNTIME_ERROR,    // Runtime execution error
    REFERENCE_ERROR,  // Variable not found
    SYNTAX_ERROR,     // Syntax/parse error
    IMPORT_ERROR,     // Module import error
    BLOCK_ERROR,      // Block execution error
    ASSERTION_ERROR   // Assertion failure
};

// Stack frame for call stack tracking
struct StackFrame {
    std::string function_name;  // Function or block name
    std::string file_path;      // Source file (empty for REPL)
    int line_number;            // Line number in source
    int column_number;          // Column number in source

    StackFrame(std::string fn, std::string fp, int line, int col = 0)
        : function_name(std::move(fn)), file_path(std::move(fp)),
          line_number(line), column_number(col) {}

    std::string toString() const;
};

// Enhanced error with stack trace - Phase 4.1
class NaabError : public std::runtime_error {
public:
    // Constructor with all fields
    NaabError(std::string message, ErrorType type = ErrorType::GENERIC,
              std::vector<StackFrame> stack = {})
        : std::runtime_error(message), message_(std::move(message)),
          error_type_(type), stack_trace_(std::move(stack)) {}

    // Constructor from thrown value (implementation in .cpp to avoid forward declaration issues)
    explicit NaabError(std::shared_ptr<Value> value);

    // Getters
    const std::string& getMessage() const { return message_; }
    ErrorType getType() const { return error_type_; }
    const std::vector<StackFrame>& getStackTrace() const { return stack_trace_; }
    std::shared_ptr<Value> getValue() const { return value_; }

    // Add frame to stack trace
    void pushFrame(const StackFrame& frame) {
        stack_trace_.push_back(frame);
    }

    // Format full error message with stack trace
    std::string formatError() const;

    // Get error type as string
    static std::string errorTypeToString(ErrorType type);

private:
    std::string message_;
    ErrorType error_type_;
    std::vector<StackFrame> stack_trace_;
    std::shared_ptr<Value> value_;  // For throw <value> support
};

#include "naab/struct_value.h"

class Value; // GEMINI FIX: Forward declaration, full definition moved to naab/value.h




// Interpreter
class Interpreter : public ast::ASTVisitor {
public:
    Interpreter();

    // Execute a program
    void execute(ast::Program& program);

    // Visitor implementations
    void visit(ast::Program& node) override;
    void visit(ast::UseStatement& node) override;
    void visit(ast::ImportStmt& node) override;  // Phase 3.1
    void visit(ast::ExportStmt& node) override;  // Phase 3.1
    void visit(ast::FunctionDecl& node) override;
    void visit(ast::StructDecl& node) override;
    void visit(ast::MainBlock& node) override;
    void visit(ast::CompoundStmt& node) override;
    void visit(ast::ExprStmt& node) override;
    void visit(ast::ReturnStmt& node) override;
    void visit(ast::IfStmt& node) override;
    void visit(ast::ForStmt& node) override;
    void visit(ast::WhileStmt& node) override;
    void visit(ast::BreakStmt& node) override;
    void visit(ast::ContinueStmt& node) override;
    void visit(ast::VarDeclStmt& node) override;
    void visit(ast::TryStmt& node) override;      // Phase 4.1
    void visit(ast::ThrowStmt& node) override;    // Phase 4.1
    void visit(ast::BinaryExpr& node) override;
    void visit(ast::UnaryExpr& node) override;
    void visit(ast::CallExpr& node) override;
    void visit(ast::MemberExpr& node) override;
    void visit(ast::IdentifierExpr& node) override;
    void visit(ast::LiteralExpr& node) override;
    void visit(ast::DictExpr& node) override;
    void visit(ast::ListExpr& node) override;
    void visit(ast::StructLiteralExpr& node) override;
    void visit(ast::InlineCodeExpr& node) override;

    // Get last evaluated value
    std::shared_ptr<Value> getResult() const { return result_; }

    // Call a function value with arguments (for higher-order functions)
    std::shared_ptr<Value> callFunction(std::shared_ptr<Value> fn,
                                        const std::vector<std::shared_ptr<Value>>& args);

    // Phase 11.1: Flush captured output from polyglot executors
    void flushExecutorOutput(runtime::Executor* executor);

    // Debugger support
    void setDebugger(std::shared_ptr<debugger::Debugger> debugger);
    std::shared_ptr<debugger::Debugger> getDebugger() const { return debugger_; }
    bool isDebugging() const { return debugger_ && debugger_->isActive(); }

    // Verbose mode support
    void setVerboseMode(bool v) { verbose_mode_ = v; }
    bool isVerboseMode() const { return verbose_mode_; }

    // Profile mode support
    void setProfileMode(bool p) { profile_mode_ = p; }
    bool isProfileMode() const { return profile_mode_; }
    void profileStart(const std::string& name);
    void profileEnd(const std::string& name);
    void printProfile() const;

    // Explain mode support
    void setExplainMode(bool e) { explain_mode_ = e; }
    bool isExplainMode() const { return explain_mode_; }
    void explain(const std::string& message) const;

private:
    std::shared_ptr<Environment> global_env_;
    std::shared_ptr<Environment> current_env_;
    std::shared_ptr<Value> result_;
    bool returning_;
    bool breaking_;
    bool continuing_;

    // Block loading
    std::unique_ptr<runtime::BlockLoader> block_loader_;
    std::unordered_map<std::string, runtime::BlockMetadata> loaded_blocks_;

    // Phase 4.4: Block pair tracking for usage analytics
    std::string last_executed_block_id_;

    // C++ block execution
    std::unique_ptr<runtime::CppExecutor> cpp_executor_;

    // Standard library
    std::unique_ptr<stdlib::StdLib> stdlib_;
    std::unordered_map<std::string, std::shared_ptr<stdlib::Module>> imported_modules_;

    // Debugger
    std::shared_ptr<debugger::Debugger> debugger_;

    // Phase 3.1: Module system
    std::unique_ptr<modules::ModuleResolver> module_resolver_;
    std::unordered_map<std::string, std::shared_ptr<Environment>> loaded_modules_;  // Module path -> exports
    std::unordered_map<std::string, std::shared_ptr<Value>> module_exports_;  // Exported symbols from current module

    // Phase 4.1: Call stack for error reporting
    std::vector<StackFrame> call_stack_;
    std::string current_file_;  // Current source file being executed

    // Verbose mode
    bool verbose_mode_ = false;

    // Profile mode
    bool profile_mode_ = false;
    std::chrono::time_point<std::chrono::high_resolution_clock> profile_start_;
    std::unordered_map<std::string, long long> profile_timings_;  // microseconds

    // Explain mode
    bool explain_mode_ = false;

    // Helpers
    std::shared_ptr<Value> eval(ast::Expr& expr);
    void executeStmt(ast::Stmt& stmt);
    void defineBuiltins();
    std::shared_ptr<Environment> loadAndExecuteModule(const std::string& module_path);  // Phase 3.1

    // Phase 4.1: Stack trace helpers
    void pushStackFrame(const std::string& function_name, int line = 0);
    void popStackFrame();
    NaabError createError(const std::string& message, ErrorType type = ErrorType::RUNTIME_ERROR);
};

} // namespace interpreter
} // namespace naab

#endif // NAAB_INTERPRETER_H
