#ifndef NAAB_TYPE_CHECKER_H
#define NAAB_TYPE_CHECKER_H

// NAAb Type Checker - Static type analysis
// Provides type inference, checking, and error reporting

#include "naab/ast.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace naab {
namespace typecheck {

// Type kinds
enum class TypeKind {
    Void,
    Int,
    Float,
    Bool,
    String,
    List,
    Dict,
    Block,
    Function,
    PythonObject,
    Any,      // For dynamic typing
    Unknown   // For inference
};

// Forward declarations
class Type;
class TypeEnvironment;

// Base type representation
class Type {
public:
    TypeKind kind;

    // For composite types
    std::shared_ptr<Type> element_type;  // For List
    std::shared_ptr<Type> key_type;      // For Dict
    std::shared_ptr<Type> value_type;    // For Dict

    // For function types
    std::vector<std::shared_ptr<Type>> param_types;
    std::shared_ptr<Type> return_type;

    explicit Type(TypeKind k) : kind(k) {}

    std::string toString() const;
    bool isCompatibleWith(const Type& other) const;
    bool isNumeric() const;

    // Factory methods
    static std::shared_ptr<Type> makeVoid();
    static std::shared_ptr<Type> makeInt();
    static std::shared_ptr<Type> makeFloat();
    static std::shared_ptr<Type> makeBool();
    static std::shared_ptr<Type> makeString();
    static std::shared_ptr<Type> makeList(std::shared_ptr<Type> elem);
    static std::shared_ptr<Type> makeDict(std::shared_ptr<Type> key, std::shared_ptr<Type> value);
    static std::shared_ptr<Type> makeFunction(
        std::vector<std::shared_ptr<Type>> params,
        std::shared_ptr<Type> ret
    );
    static std::shared_ptr<Type> makeBlock();
    static std::shared_ptr<Type> makePythonObject();
    static std::shared_ptr<Type> makeAny();
    static std::shared_ptr<Type> makeUnknown();
};

// Type error information
struct TypeError {
    std::string message;
    size_t line;
    size_t column;
    std::string context;  // Code snippet

    TypeError(const std::string& msg, size_t l, size_t c, const std::string& ctx = "")
        : message(msg), line(l), column(c), context(ctx) {}

    std::string toString() const;
};

// Type environment for scoped type tracking
class TypeEnvironment {
public:
    TypeEnvironment() : parent_(nullptr) {}
    explicit TypeEnvironment(std::shared_ptr<TypeEnvironment> parent) : parent_(parent) {}

    void define(const std::string& name, std::shared_ptr<Type> type);
    std::shared_ptr<Type> get(const std::string& name);
    void set(const std::string& name, std::shared_ptr<Type> type);
    bool has(const std::string& name) const;

private:
    std::unordered_map<std::string, std::shared_ptr<Type>> types_;
    std::shared_ptr<TypeEnvironment> parent_;
};

// Type checker visitor
class TypeChecker : public ast::ASTVisitor {
public:
    TypeChecker();

    // Check a program and return type errors
    std::vector<TypeError> check(std::shared_ptr<ast::Program> program);

    // Get inferred type of last expression
    std::shared_ptr<Type> getLastType() const { return current_type_; }

    // AST Visitor overrides
    void visit(ast::Program& node) override;
    void visit(ast::UseStatement& node) override;
    void visit(ast::FunctionDecl& node) override;
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
    void visit(ast::ImportStmt& node) override;   // Phase 3.1
    void visit(ast::ExportStmt& node) override;   // Phase 3.1
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

private:
    std::shared_ptr<TypeEnvironment> env_;
    std::shared_ptr<Type> current_type_;
    std::vector<TypeError> errors_;

    // Current function return type (for return statement checking)
    std::shared_ptr<Type> current_function_return_type_;

    // Type inference helpers
    std::shared_ptr<Type> inferBinaryOpType(
        const std::string& op,
        std::shared_ptr<Type> left,
        std::shared_ptr<Type> right,
        size_t line, size_t column
    );

    std::shared_ptr<Type> inferUnaryOpType(
        const std::string& op,
        std::shared_ptr<Type> operand,
        size_t line, size_t column
    );

    // Type checking helpers
    bool checkTypeCompatibility(
        std::shared_ptr<Type> expected,
        std::shared_ptr<Type> actual,
        const std::string& context,
        size_t line, size_t column
    );

    void reportError(const std::string& message, size_t line, size_t column);

    // Environment management
    void pushScope();
    void popScope();
};

} // namespace typecheck
} // namespace naab

#endif // NAAB_TYPE_CHECKER_H
