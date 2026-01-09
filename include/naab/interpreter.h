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
#include <Python.h>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>

namespace naab {

namespace interpreter {

// Forward declarations
class Value;
class Environment;

// Block wrapper for loaded blocks (multi-language support)
struct BlockValue {
    runtime::BlockMetadata metadata;
    std::string code;
    std::string python_namespace;  // Python namespace name for this block (Python only)
    std::string member_path;        // For member access like "Class.method"

    // Phase 7: Executor support (either owned or borrowed)
    runtime::Executor* executor_;              // Borrowed executor (for shared executors like JS)
    std::unique_ptr<runtime::Executor> owned_executor_;  // Owned executor (for C++ blocks)

    BlockValue(const runtime::BlockMetadata& meta, const std::string& c,
               const std::string& ns = "", const std::string& mp = "")
        : metadata(meta), code(c), python_namespace(ns), member_path(mp), executor_(nullptr) {}

    // Phase 7: Constructor with borrowed executor (for JS, Python, etc.)
    BlockValue(const runtime::BlockMetadata& meta, const std::string& c,
               runtime::Executor* exec)
        : metadata(meta), code(c), python_namespace(""), member_path(""), executor_(exec) {}

    // Phase 7: Constructor with owned executor (for C++)
    BlockValue(const runtime::BlockMetadata& meta, const std::string& c,
               std::unique_ptr<runtime::Executor> exec)
        : metadata(meta), code(c), python_namespace(""), member_path(""),
          executor_(nullptr), owned_executor_(std::move(exec)) {}

    // Phase 7: Get active executor (owned or borrowed)
    runtime::Executor* getExecutor() const {
        return owned_executor_ ? owned_executor_.get() : executor_;
    }
};

// Function wrapper for user-defined functions
struct FunctionValue {
    std::string name;
    std::vector<std::string> params;
    std::vector<ast::Expr*> defaults;  // Default values for parameters (non-owning pointers)
    std::shared_ptr<ast::CompoundStmt> body;

    FunctionValue(const std::string& n,
                  const std::vector<std::string>& p,
                  std::vector<ast::Expr*> d,
                  std::shared_ptr<ast::CompoundStmt> b)
        : name(n), params(p), defaults(std::move(d)), body(b) {}
};

// Python object wrapper for method chaining
struct PythonObjectValue {
    PyObject* obj;       // Owned reference
    std::string repr;    // String representation for display

    explicit PythonObjectValue(PyObject* o) : obj(o), repr("<python-object>") {
        if (obj) {
            Py_INCREF(obj);  // Take ownership
            // Get string representation
            PyObject* str_obj = PyObject_Str(obj);
            if (str_obj) {
                const char* str_val = PyUnicode_AsUTF8(str_obj);
                if (str_val) repr = str_val;
                Py_DECREF(str_obj);
            }
        }
    }

    ~PythonObjectValue() {
        if (obj) {
            Py_DECREF(obj);  // Release reference
        }
    }

    // Delete copy constructor/assignment to prevent double-free
    PythonObjectValue(const PythonObjectValue&) = delete;
    PythonObjectValue& operator=(const PythonObjectValue&) = delete;

    // Allow move
    PythonObjectValue(PythonObjectValue&& other) noexcept
        : obj(other.obj), repr(std::move(other.repr)) {
        other.obj = nullptr;
    }

    PythonObjectValue& operator=(PythonObjectValue&& other) noexcept {
        if (this != &other) {
            if (obj) Py_DECREF(obj);
            obj = other.obj;
            repr = std::move(other.repr);
            other.obj = nullptr;
        }
        return *this;
    }
};

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

// Forward declarations for struct types
struct StructDef;
struct StructValue;

// Struct type definition
struct StructDef {
    std::string name;
    std::vector<ast::StructField> fields;
    std::unordered_map<std::string, size_t> field_index;

    StructDef() = default;
    StructDef(std::string n, std::vector<ast::StructField> f)
        : name(std::move(n)), fields(std::move(f)) {
        for (size_t i = 0; i < fields.size(); ++i) {
            field_index[fields[i].name] = i;
        }
    }
};

// Struct instance value
struct StructValue {
    std::string type_name;
    std::shared_ptr<StructDef> definition;
    std::vector<std::shared_ptr<Value>> field_values;

    StructValue() = default;
    StructValue(std::string name, std::shared_ptr<StructDef> def)
        : type_name(std::move(name)), definition(def) {
        if (def) {
            field_values.resize(def->fields.size());
        }
    }

    std::shared_ptr<Value> getField(const std::string& name) const;
    void setField(const std::string& name, std::shared_ptr<Value> value);
};

// Runtime value types
using ValueData = std::variant<
    std::monostate,  // void/null (index 0)
    int,             // index 1
    double,          // index 2
    bool,            // index 3
    std::string,     // index 4
    std::vector<std::shared_ptr<Value>>,  // list (index 5)
    std::unordered_map<std::string, std::shared_ptr<Value>>,  // dict (index 6)
    std::shared_ptr<BlockValue>,  // block (index 7)
    std::shared_ptr<FunctionValue>,  // function (index 8)
    std::shared_ptr<PythonObjectValue>,  // python object (index 9)
    std::shared_ptr<StructValue>  // struct (index 10)
>;

class Value {
public:
    ValueData data;

    Value() : data(std::monostate{}) {}
    explicit Value(int v) : data(v) {}
    explicit Value(double v) : data(v) {}
    explicit Value(bool v) : data(v) {}
    explicit Value(std::string v) : data(std::move(v)) {}
    explicit Value(std::vector<std::shared_ptr<Value>> v) : data(std::move(v)) {}
    explicit Value(std::unordered_map<std::string, std::shared_ptr<Value>> v) : data(std::move(v)) {}
    explicit Value(std::shared_ptr<BlockValue> v) : data(std::move(v)) {}
    explicit Value(std::shared_ptr<FunctionValue> v) : data(std::move(v)) {}
    explicit Value(std::shared_ptr<PythonObjectValue> v) : data(std::move(v)) {}
    explicit Value(std::shared_ptr<StructValue> v) : data(std::move(v)) {}

    std::string toString() const;
    bool toBool() const;
    int toInt() const;
    double toFloat() const;
};

// Variable environment (scoping)
class Environment {
public:
    Environment() : parent_(nullptr) {}
    explicit Environment(std::shared_ptr<Environment> parent) : parent_(parent) {}

    void define(const std::string& name, std::shared_ptr<Value> value);
    std::shared_ptr<Value> get(const std::string& name);
    void set(const std::string& name, std::shared_ptr<Value> value);
    bool has(const std::string& name) const;
    std::vector<std::string> getAllNames() const;

    // Exported structs from this module (Week 7)
    std::unordered_map<std::string, std::shared_ptr<StructDef>> exported_structs_;

private:
    std::unordered_map<std::string, std::shared_ptr<Value>> values_;
    std::shared_ptr<Environment> parent_;
};

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

    // Get last evaluated value
    std::shared_ptr<Value> getResult() const { return result_; }

    // Call a function value with arguments (for higher-order functions)
    std::shared_ptr<Value> callFunction(std::shared_ptr<Value> fn,
                                        const std::vector<std::shared_ptr<Value>>& args);

    // Debugger support
    void setDebugger(std::shared_ptr<debugger::Debugger> debugger);
    std::shared_ptr<debugger::Debugger> getDebugger() const { return debugger_; }
    bool isDebugging() const { return debugger_ && debugger_->isActive(); }

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
