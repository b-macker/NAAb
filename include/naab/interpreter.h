#pragma once

// NAAb Block Assembly Language - Interpreter
// Direct AST execution with visitor pattern

#include "naab/ast.h"
#include "naab/block_loader.h"
#include "naab/stdlib.h"
#include "naab/cpp_executor.h"
#include "naab/language_registry.h"
#include "naab/debugger.h"
#include "naab/module_resolver.h"  // Phase 3.1
#include "naab/module_system.h"    // Phase 4.0: Module registry
#include "naab/error_reporter.h"    // Phase 3.1: Enhanced error messages
#include "naab/suggestion_system.h" // Phase 3.1: "Did you mean?" suggestions
#include <Python.h>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>

// Forward declare CycleDetector
namespace naab {
namespace interpreter {
class CycleDetector;
struct DependencyGroup;  // From polyglot_dependency_analyzer.h
}
}

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
    std::vector<ast::Type> param_types;  // Phase 2.1: Parameter types (for ref checking)
    std::vector<ast::Expr*> defaults;  // Default values for parameters (non-owning pointers)
    std::shared_ptr<ast::CompoundStmt> body;
    std::vector<std::string> type_parameters;  // Phase 2.4.1: Generic type parameters (T, U, etc.)
    ast::Type return_type;  // Phase 2.4.2: Return type for validation
    std::string source_file;  // Phase 3.1: Source file for stack traces
    int source_line;  // Phase 3.1: Line number for stack traces
    std::shared_ptr<Environment> closure;  // ISS-022: Closure environment for lexical scoping
    bool is_async = false;  // Phase 6: async function flag

    FunctionValue(const std::string& n,
                  const std::vector<std::string>& p,
                  std::vector<ast::Type> pt,
                  std::vector<ast::Expr*> d,
                  std::shared_ptr<ast::CompoundStmt> b,
                  std::vector<std::string> tp = {},
                  ast::Type rt = ast::Type::makeAny(),
                  std::string sf = "",
                  int sl = 0,
                  std::shared_ptr<Environment> cls = nullptr,
                  bool async = false)
        : name(n), params(p), param_types(std::move(pt)), defaults(std::move(d)), body(b), type_parameters(std::move(tp)), return_type(std::move(rt)), source_file(std::move(sf)), source_line(sl), closure(cls), is_async(async) {}
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

// Forward declarations for struct and enum types
struct StructDef;
struct StructValue;
struct EnumDef;

// Struct type definition
struct StructDef {
    std::string name;
    std::vector<ast::StructField> fields;
    std::unordered_map<std::string, size_t> field_index;
    std::vector<std::string> type_parameters;  // Phase 2.4.1: Generic type parameters (T, U, etc.)

    StructDef() = default;
    StructDef(std::string n, std::vector<ast::StructField> f,
              std::vector<std::string> tp = {})
        : name(std::move(n)), fields(std::move(f)), type_parameters(std::move(tp)) {
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

    // Optimized: inline for zero call overhead
    inline std::shared_ptr<Value> getField(const std::string& name) const {
        if (!definition) [[unlikely]] {
            throw std::runtime_error("Struct has no definition");
        }
        auto it = definition->field_index.find(name);
        if (it == definition->field_index.end()) [[unlikely]] {
            throw std::runtime_error("Field '" + name +
                                   "' not found in struct '" + type_name + "'");
        }
        return field_values[it->second];
    }
    // Optimized: inline for zero call overhead
    inline void setField(const std::string& name, std::shared_ptr<Value> value) {
        if (!definition) [[unlikely]] {
            throw std::runtime_error("Struct has no definition");
        }
        auto it = definition->field_index.find(name);
        if (it == definition->field_index.end()) [[unlikely]] {
            throw std::runtime_error("Field '" + name +
                                   "' not found in struct '" + type_name + "'");
        }
        field_values[it->second] = value;
    }

    // Fast path: direct indexed access (bypasses hash lookup)
    inline std::shared_ptr<Value> getFieldByIndex(size_t index) const {
        if (index >= field_values.size()) [[unlikely]] {
            throw std::runtime_error("Field index out of bounds");
        }
        return field_values[index];
    }

    inline void setFieldByIndex(size_t index, std::shared_ptr<Value> value) {
        if (index >= field_values.size()) [[unlikely]] {
            throw std::runtime_error("Field index out of bounds");
        }
        field_values[index] = value;
    }

    // Get field index by name (for caching)
    inline size_t getFieldIndex(const std::string& name) const {
        if (!definition) [[unlikely]] {
            throw std::runtime_error("Struct has no definition");
        }
        auto it = definition->field_index.find(name);
        if (it == definition->field_index.end()) [[unlikely]] {
            throw std::runtime_error("Field '" + name +
                                   "' not found in struct '" + type_name + "'");
        }
        return it->second;
    }
};

// Enum type definition (Phase 4.1: Module System)
struct EnumDef {
    std::string name;
    std::vector<std::pair<std::string, int>> variants;  // variant_name -> value
    std::unordered_map<std::string, int> variant_values;

    EnumDef() = default;
    EnumDef(std::string n, std::vector<std::pair<std::string, int>> v)
        : name(std::move(n)), variants(std::move(v)) {
        for (const auto& [variant_name, value] : variants) {
            variant_values[variant_name] = value;
        }
    }
};

// Future value for async function results
struct FutureValue {
    std::shared_future<std::shared_ptr<Value>> future;
    std::string description;  // for error messages (e.g., "async fn fetch_data")
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
    std::shared_ptr<StructValue>,  // struct (index 10)
    std::shared_ptr<FutureValue>  // future (index 11) - Phase 6: async/await
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
    explicit Value(std::shared_ptr<FutureValue> v) : data(std::move(v)) {}

    std::string toString() const;
    bool toBool() const;
    int toInt() const;
    double toFloat() const;

    // Phase 3.2: Cycle detection support - traverse all referenced values
    void traverse(std::function<void(std::shared_ptr<Value>)> visitor) const;
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

    // Phase 3.2: GC support - access to values for cycle detection
    const std::unordered_map<std::string, std::shared_ptr<Value>>& getValues() const { return values_; }
    std::shared_ptr<Environment> getParent() const { return parent_; }

    // Exported structs from this module (Week 7)
    std::unordered_map<std::string, std::shared_ptr<StructDef>> exported_structs_;

    // Exported enums from this module (Phase 4.1: Module System)
    std::unordered_map<std::string, std::shared_ptr<EnumDef>> exported_enums_;

private:
    std::unordered_map<std::string, std::shared_ptr<Value>> values_;
    std::shared_ptr<Environment> parent_;
};

// Interpreter
class Interpreter : public ast::ASTVisitor {
public:
    Interpreter();
    ~Interpreter();  // Phase 3.2: Declared here, defined in .cpp (for unique_ptr<CycleDetector>)

    // Execute a program
    void execute(ast::Program& program);

    // Phase 3.1: Set source code for enhanced error messages
    void setSourceCode(const std::string& source, const std::string& filename = "");

    // Visitor implementations
    void visit(ast::Program& node) override;
    void visit(ast::UseStatement& node) override;
    void visit(ast::ModuleUseStmt& node) override;  // Phase 4.0
    void visit(ast::ImportStmt& node) override;  // Phase 3.1
    void visit(ast::ExportStmt& node) override;  // Phase 3.1
    void visit(ast::FunctionDecl& node) override;
    void visit(ast::StructDecl& node) override;
    void visit(ast::EnumDecl& node) override;  // Phase 2.4.3
    void visit(ast::FunctionDeclStmt& node) override;  // Nested function declaration
    void visit(ast::StructDeclStmt& node) override;    // Nested struct declaration
    void visit(ast::RuntimeDeclStmt& node) override;   // Phase 12: Persistent runtime
    void visit(ast::MainBlock& node) override;
    void visit(ast::CompoundStmt& node) override;
    void visit(ast::ExprStmt& node) override;
    void visit(ast::ReturnStmt& node) override;
    void visit(ast::IfStmt& node) override;
    void visit(ast::IfExpr& node) override;
    void visit(ast::MatchExpr& node) override;
    void visit(ast::AwaitExpr& node) override;
    void visit(ast::LambdaExpr& node) override;
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
    void visit(ast::RangeExpr& node) override;
    void visit(ast::StructLiteralExpr& node) override;
    void visit(ast::InlineCodeExpr& node) override;

    // Get last evaluated value
    std::shared_ptr<Value> getResult() const { return result_; }

    // Phase 6: Async execution support
    void setGlobalEnv(std::shared_ptr<Environment> env) { global_env_ = env; }
    void setCurrentEnv(std::shared_ptr<Environment> env) { current_env_ = env; }
    std::shared_ptr<Value> executeBodyInEnv(ast::CompoundStmt& body, std::shared_ptr<Environment> env);

    // Call a function value with arguments (for higher-order functions)
    std::shared_ptr<Value> callFunction(std::shared_ptr<Value> fn,
                                        const std::vector<std::shared_ptr<Value>>& args);

    // Phase 11.1: Flush captured output from polyglot executors
    void flushExecutorOutput(runtime::Executor* executor);

    // Phase 12: Polyglot header-aware injection
    std::string injectDeclarationsAfterHeaders(const std::string& declarations,
                                                const std::string& code,
                                                const std::string& language);

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

    // Phase 3.2: Garbage collection support
    void runGarbageCollection(std::shared_ptr<Environment> env = nullptr);
    void setGCEnabled(bool enabled) { gc_enabled_ = enabled; }
    bool isGCEnabled() const { return gc_enabled_; }
    void setGCThreshold(size_t threshold) { gc_threshold_ = threshold; }
    size_t getAllocationCount() const { return allocation_count_; }
    size_t getGCCollectionCount() const;
    void registerValue(std::shared_ptr<Value> value);  // Track value for complete GC

    // Command-line arguments support (ISS-028)
    void setScriptArgs(const std::vector<std::string>& args) { script_args_ = args; }
    const std::vector<std::string>& getScriptArgs() const { return script_args_; }

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

    // Phase 3.1: Module system (ES6-style imports)
    std::unique_ptr<modules::ModuleResolver> module_resolver_;
    std::unordered_map<std::string, std::shared_ptr<Environment>> loaded_modules_;  // Module path -> exports
    std::unordered_map<std::string, std::shared_ptr<Value>> module_exports_;  // Exported symbols from current module

    // Phase 4.0: Module system (Rust-style use statements)
    std::unique_ptr<modules::ModuleRegistry> module_registry_;

    // Phase 4.1: Call stack for error reporting
    std::vector<StackFrame> call_stack_;
    std::string current_file_;  // Current source file being executed

    // Week 1, Task 1.3: Track call depth to prevent stack overflow
    size_t call_depth_ = 0;

    // Phase 2.4.2: Track current function for return type validation
    std::shared_ptr<FunctionValue> current_function_;

    // Phase 2.4.4: Track current type substitutions for generic functions
    std::map<std::string, ast::Type> current_type_substitutions_;

    // Verbose mode
    bool verbose_mode_ = false;

    // Profile mode
    bool profile_mode_ = false;
    std::chrono::time_point<std::chrono::high_resolution_clock> profile_start_;
    std::unordered_map<std::string, long long> profile_timings_;  // microseconds

    // Explain mode
    bool explain_mode_ = false;

    // Phase 3.2: Garbage collection
    std::unique_ptr<CycleDetector> cycle_detector_;
    bool gc_enabled_ = true;  // GC enabled by default
    bool gc_suspended_ = false;  // Temporarily suspend GC (e.g., during polyglot execution)
    size_t gc_threshold_ = 5000;  // Run GC every N allocations
    size_t allocation_count_ = 0;
    std::vector<std::weak_ptr<Value>> tracked_values_;  // Global value tracking for complete GC

    // Phase 3.1: Enhanced error reporting
    error::ErrorReporter error_reporter_;
    std::string source_code_;  // Source code for error context

    // Command-line arguments (ISS-028)
    std::vector<std::string> script_args_;

    // Loop depth tracking for break/continue validation
    int loop_depth_;

    // Phase 12: Persistent sub-runtime contexts
    // Maps runtime name -> {language, executor, code_buffer (for subprocess langs)}
    struct PersistentRuntime {
        std::string language;
        std::shared_ptr<runtime::Executor> executor;
        std::string code_buffer;  // Accumulated code for subprocess-based languages
    };
    std::unordered_map<std::string, PersistentRuntime> named_runtimes_;

    // File context tracking for relative imports
    std::vector<std::filesystem::path> file_context_stack_;

    // Nested types for parallel execution
    struct VariableSnapshot {
        std::unordered_map<std::string, std::shared_ptr<Value>> variables;

        void capture(
            Environment* env,
            const std::vector<std::string>& var_names,
            Interpreter* interp
        );
    };

    // Helpers
    std::shared_ptr<Value> eval(ast::Expr& expr);
    void executeStmt(ast::Stmt& stmt);
    void defineBuiltins();
    std::shared_ptr<Environment> loadAndExecuteModule(const std::string& module_path);  // Phase 3.1
    std::shared_ptr<Value> copyValue(const std::shared_ptr<Value>& value);  // Phase 2.1: Deep copy for value parameters
    std::string serializeValueForLanguage(const std::shared_ptr<Value>& value, const std::string& language);  // Phase 2.2: Serialize value for target language

    // File context management for relative imports
    void pushFileContext(const std::filesystem::path& file_path);
    void popFileContext();
    std::filesystem::path getCurrentFileDirectory() const;

    // Parallel polyglot execution
    void executePolyglotGroupParallel(const DependencyGroup& group);

    // Variable access helper
    std::shared_ptr<Value> getVariable(const std::string& name) const;

    // Path resolution helper
    std::filesystem::path resolveRelativePath(const std::string& path) const;

    // Phase 4.1: Stack trace helpers
    void pushStackFrame(const std::string& function_name, int line = 0);
    void popStackFrame();
    NaabError createError(const std::string& message, ErrorType type = ErrorType::RUNTIME_ERROR);

    // Phase 3.2: GC helpers
    void trackAllocation();
    std::vector<std::weak_ptr<Value>>& getTrackedValues() { return tracked_values_; }

    // Phase 2.4.1: Generics/Monomorphization helpers
    ast::Type inferValueType(const std::shared_ptr<Value>& value);
    std::map<std::string, ast::Type> inferTypeBindings(
        const std::vector<std::string>& type_params,
        const std::vector<ast::StructField>& fields,
        const std::vector<std::pair<std::string, std::unique_ptr<ast::Expr>>>& field_inits
    );
    ast::Type substituteType(const ast::Type& type, const std::map<std::string, ast::Type>& bindings);
    std::shared_ptr<StructDef> monomorphizeStruct(
        const std::shared_ptr<StructDef>& generic_def,
        const std::map<std::string, ast::Type>& type_bindings
    );

    // Phase 2.4.2: Union type validation helpers
    bool valueMatchesType(const std::shared_ptr<Value>& value, const ast::Type& type);
    bool valueMatchesUnion(const std::shared_ptr<Value>& value, const std::vector<ast::Type>& union_types);
    std::string getValueTypeName(const std::shared_ptr<Value>& value);
    std::string formatTypeName(const ast::Type& type);

    // Phase 2.4.5: Null safety helpers
    bool isNull(const std::shared_ptr<Value>& value);

    // Phase 2.4.4: Type inference helpers
    ast::Type inferTypeFromValue(const std::shared_ptr<Value>& value);

    // Phase 2.4.4 Phase 2: Function return type inference
    ast::Type inferReturnType(ast::Stmt* body);
    void collectReturnTypes(ast::Stmt* stmt, std::vector<ast::Type>& return_types);

    // Phase 2.4.4 Phase 3: Generic argument inference
    std::vector<ast::Type> inferGenericArgs(
        const std::shared_ptr<FunctionValue>& func,
        const std::vector<std::shared_ptr<Value>>& args
    );
    void collectTypeConstraints(
        const ast::Type& param_type,
        const ast::Type& arg_type,
        std::map<std::string, ast::Type>& constraints
    );
    ast::Type substituteTypeParams(
        const ast::Type& type,
        const std::map<std::string, ast::Type>& substitutions
    );
};

} // namespace interpreter
} // namespace naab

