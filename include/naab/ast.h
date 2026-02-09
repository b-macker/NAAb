#ifndef NAAB_AST_H
#define NAAB_AST_H

// NAAb Block Assembly Language - Abstract Syntax Tree
// Adapted from Clang AST blocks (BLOCK-CPP-07000+)

#include <memory>
#include <string>
#include <vector>
#include <optional>

namespace naab {
namespace ast {

// Forward declarations
class ASTVisitor;
class Expr;
class Stmt;
class StructDecl;
class StructLiteralExpr;
class InlineCodeExpr;

// Source location for error reporting
struct SourceLocation {
    int line;
    int column;
    std::string filename;

    SourceLocation(int l = 0, int c = 0, const std::string& f = "")
        : line(l), column(c), filename(f) {}
};

// ============================================================================
// Base AST Node (inspired by Clang's Stmt/Expr hierarchy)
// ============================================================================

enum class NodeKind {
    // Program
    Program,

    // Declarations
    UseStatement,
    FunctionDecl,
    MainBlock,
    StructDecl,           // Struct declaration
    EnumDecl,             // Phase 2.4.3: Enum declaration

    // Statements
    CompoundStmt,
    ExprStmt,
    ReturnStmt,
    IfStmt,
    ForStmt,
    WhileStmt,
    BreakStmt,
    ContinueStmt,
    VarDeclStmt,
    ImportStmt,     // Phase 3.1: import statement
    ExportStmt,     // Phase 3.1: export statement
    TryStmt,        // Phase 4.1: try statement
    ThrowStmt,      // Phase 4.1: throw statement
    ModuleUseStmt,  // Phase 4.0: module use statement (use math_utils)

    // Expressions
    BinaryExpr,
    UnaryExpr,
    CallExpr,
    MemberExpr,
    IdentifierExpr,
    LiteralExpr,
    DictExpr,
    ListExpr,
    RangeExpr,            // Range operator: start..end
    StructLiteralExpr,    // Struct literal expression
    InlineCodeExpr,       // Inline polyglot code: <<language code >>
};

class ASTNode {
public:
    explicit ASTNode(NodeKind kind, SourceLocation loc = SourceLocation())
        : kind_(kind), loc_(loc) {}

    virtual ~ASTNode() = default;

    NodeKind getKind() const { return kind_; }
    SourceLocation getLocation() const { return loc_; }

    virtual void accept(ASTVisitor& visitor) = 0;

private:
    NodeKind kind_;
    SourceLocation loc_;
};

// ============================================================================
// Type System
// ============================================================================

enum class TypeKind {
    Void,
    Int,
    Float,
    String,
    Bool,
    Dict,
    List,
    Any,
    Block,  // Block reference type
    Struct, // Struct type
    Function,  // P3 ISS-002: Function type
    TypeParameter,  // Phase 2.4.1: Generic type parameter (T, U, etc.)
    Union,  // Phase 2.4.2: Union type (int | string)
    Enum,   // Phase 4.1: Enum type
};

struct Type {
    TypeKind kind;
    std::shared_ptr<Type> element_type;  // For list[T]
    std::shared_ptr<std::pair<Type, Type>> key_value_types;  // For dict[K,V]
    std::string struct_name;  // For struct types
    std::string enum_name;    // Phase 4.1: For enum types
    std::string module_prefix;  // ISS-024 Fix: For module-qualified types (module.Type)
    bool is_nullable;  // For nullable types (?Type)
    bool is_reference;  // Phase 2.1: For reference types (ref Type)
    std::vector<Type> type_arguments;  // Phase 2.4.1: For generic types (List<int>)
    std::string type_parameter_name;  // Phase 2.4.1: If this is a type parameter reference (T, U)
    std::vector<Type> union_types;  // Phase 2.4.2: For union types (int | string)

    explicit Type(TypeKind k, std::string sn = "", bool nullable = false, bool reference = false)
        : kind(k), element_type(nullptr), key_value_types(nullptr), struct_name(std::move(sn)), is_nullable(nullable), is_reference(reference) {}

    static Type makeVoid() { return Type(TypeKind::Void); }
    static Type makeInt() { return Type(TypeKind::Int); }
    static Type makeFloat() { return Type(TypeKind::Float); }
    static Type makeString() { return Type(TypeKind::String); }
    static Type makeBool() { return Type(TypeKind::Bool); }
    static Type makeAny() { return Type(TypeKind::Any); }
    static Type makeBlock() { return Type(TypeKind::Block); }
    static Type makeFunction() { return Type(TypeKind::Function); }
    static Type makeStruct(std::string name) { return Type(TypeKind::Struct, std::move(name)); }
    static Type makeEnum(std::string name) {
        Type t(TypeKind::Enum);
        t.enum_name = std::move(name);
        return t;
    }

    const std::string& getStructName() const { return struct_name; }
    const std::string& getEnumName() const { return enum_name; }
};

// Struct field definition
struct StructField {
    std::string name;
    Type type;
    std::optional<std::unique_ptr<Expr>> default_value;
};

// ============================================================================
// Declarations
// ============================================================================

// use BLOCK-CPP-03845 as Cord
class UseStatement : public ASTNode {
public:
    UseStatement(const std::string& block_id, const std::string& alias,
                 SourceLocation loc = SourceLocation())
        : ASTNode(NodeKind::UseStatement, loc),
          block_id_(block_id), alias_(alias) {}

    const std::string& getBlockId() const { return block_id_; }
    const std::string& getAlias() const { return alias_; }

    void accept(ASTVisitor& visitor) override;

private:
    std::string block_id_;
    std::string alias_;
};

// function process_data(input: string) -> dict { ... }
struct Parameter {
    std::string name;
    Type type;
    std::optional<std::unique_ptr<Expr>> default_value;
};

class FunctionDecl : public ASTNode {
public:
    FunctionDecl(const std::string& name,
                 std::vector<Parameter> params,
                 Type return_type,
                 std::unique_ptr<Stmt> body,
                 std::vector<std::string> type_params = {},  // Phase 2.4.1: Generic type parameters
                 bool is_async = false,  // Phase 6 (deferred): async functions
                 SourceLocation loc = SourceLocation())
        : ASTNode(NodeKind::FunctionDecl, loc),
          name_(name), params_(std::move(params)),
          return_type_(return_type), body_(std::move(body)),
          type_params_(std::move(type_params)), is_async_(is_async) {}

    const std::string& getName() const { return name_; }
    const std::vector<Parameter>& getParams() const { return params_; }
    Type getReturnType() const { return return_type_; }
    Stmt* getBody() const { return body_.get(); }
    const std::vector<std::string>& getTypeParams() const { return type_params_; }  // Phase 2.4.1
    bool isAsync() const { return is_async_; }  // Phase 6 (deferred)

    void accept(ASTVisitor& visitor) override;

private:
    std::string name_;
    std::vector<Parameter> params_;
    Type return_type_;
    std::unique_ptr<Stmt> body_;
    std::vector<std::string> type_params_;  // Phase 2.4.1: Generic type parameters (T, U, etc.)
    bool is_async_;  // Phase 6 (deferred): async function flag
};

// main { ... }
class MainBlock : public ASTNode {
public:
    explicit MainBlock(std::unique_ptr<Stmt> body,
                      SourceLocation loc = SourceLocation())
        : ASTNode(NodeKind::MainBlock, loc), body_(std::move(body)) {}

    Stmt* getBody() const { return body_.get(); }

    void accept(ASTVisitor& visitor) override;

private:
    std::unique_ptr<Stmt> body_;
};

// struct Name { field: Type; ... }
class StructDecl : public ASTNode {
public:
    StructDecl(std::string name, std::vector<StructField> fields,
               std::vector<std::string> type_params,  // Phase 2.4.1: Generic type parameters
               SourceLocation loc)
        : ASTNode(NodeKind::StructDecl, loc),
          name_(std::move(name)), fields_(std::move(fields)),
          type_params_(std::move(type_params)) {}

    const std::string& getName() const { return name_; }
    const std::vector<StructField>& getFields() const { return fields_; }
    const std::vector<std::string>& getTypeParams() const { return type_params_; }  // Phase 2.4.1

    void accept(ASTVisitor& visitor) override;

private:
    std::string name_;
    std::vector<StructField> fields_;
    std::vector<std::string> type_params_;  // Phase 2.4.1: Generic type parameters (T, U, etc.)
};

// Phase 2.4.3: enum Name { Variant1, Variant2, ... }
class EnumDecl : public ASTNode {
public:
    struct EnumVariant {
        std::string name;
        std::optional<int> value;  // Optional explicit value

        EnumVariant(std::string n, std::optional<int> v = std::nullopt)
            : name(std::move(n)), value(v) {}
    };

    EnumDecl(std::string name, std::vector<EnumVariant> variants,
             SourceLocation loc = SourceLocation())
        : ASTNode(NodeKind::EnumDecl, loc),
          name_(std::move(name)), variants_(std::move(variants)) {}

    const std::string& getName() const { return name_; }
    const std::vector<EnumVariant>& getVariants() const { return variants_; }

    void accept(ASTVisitor& visitor) override;

private:
    std::string name_;
    std::vector<EnumVariant> variants_;
};

// ============================================================================
// Statements
// ============================================================================

class Stmt : public ASTNode {
public:
    explicit Stmt(NodeKind kind, SourceLocation loc = SourceLocation())
        : ASTNode(kind, loc) {}
};

// { stmt1; stmt2; ... }
class CompoundStmt : public Stmt {
public:
    explicit CompoundStmt(std::vector<std::unique_ptr<Stmt>> stmts,
                         SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::CompoundStmt, loc), stmts_(std::move(stmts)) {}

    const std::vector<std::unique_ptr<Stmt>>& getStatements() const {
        return stmts_;
    }

    void accept(ASTVisitor& visitor) override;

private:
    std::vector<std::unique_ptr<Stmt>> stmts_;
};

// expression;
class ExprStmt : public Stmt {
public:
    explicit ExprStmt(std::unique_ptr<Expr> expr,
                     SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::ExprStmt, loc), expr_(std::move(expr)) {}

    Expr* getExpr() const { return expr_.get(); }

    void accept(ASTVisitor& visitor) override;

private:
    std::unique_ptr<Expr> expr_;
};

// return expr;
class ReturnStmt : public Stmt {
public:
    explicit ReturnStmt(std::unique_ptr<Expr> expr = nullptr,
                       SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::ReturnStmt, loc), expr_(std::move(expr)) {}

    Expr* getExpr() const { return expr_.get(); }

    void accept(ASTVisitor& visitor) override;

private:
    std::unique_ptr<Expr> expr_;
};

// if (cond) { then_branch } else { else_branch }
class IfStmt : public Stmt {
public:
    IfStmt(std::unique_ptr<Expr> cond,
           std::unique_ptr<Stmt> then_branch,
           std::unique_ptr<Stmt> else_branch = nullptr,
           SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::IfStmt, loc),
          cond_(std::move(cond)),
          then_branch_(std::move(then_branch)),
          else_branch_(std::move(else_branch)) {}

    Expr* getCondition() const { return cond_.get(); }
    Stmt* getThenBranch() const { return then_branch_.get(); }
    Stmt* getElseBranch() const { return else_branch_.get(); }

    void accept(ASTVisitor& visitor) override;

private:
    std::unique_ptr<Expr> cond_;
    std::unique_ptr<Stmt> then_branch_;
    std::unique_ptr<Stmt> else_branch_;
};

// for (var in expr) { body }
class ForStmt : public Stmt {
public:
    ForStmt(const std::string& var,
            std::unique_ptr<Expr> iter,
            std::unique_ptr<Stmt> body,
            SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::ForStmt, loc),
          var_(var), iter_(std::move(iter)), body_(std::move(body)) {}

    const std::string& getVar() const { return var_; }
    Expr* getIter() const { return iter_.get(); }
    Stmt* getBody() const { return body_.get(); }

    void accept(ASTVisitor& visitor) override;

private:
    std::string var_;
    std::unique_ptr<Expr> iter_;
    std::unique_ptr<Stmt> body_;
};

// while (cond) { body }
class WhileStmt : public Stmt {
public:
    WhileStmt(std::unique_ptr<Expr> cond,
              std::unique_ptr<Stmt> body,
              SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::WhileStmt, loc),
          cond_(std::move(cond)), body_(std::move(body)) {}

    Expr* getCondition() const { return cond_.get(); }
    Stmt* getBody() const { return body_.get(); }

    void accept(ASTVisitor& visitor) override;

private:
    std::unique_ptr<Expr> cond_;
    std::unique_ptr<Stmt> body_;
};

// break;
class BreakStmt : public Stmt {
public:
    explicit BreakStmt(SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::BreakStmt, loc) {}

    void accept(ASTVisitor& visitor) override;
};

// continue;
class ContinueStmt : public Stmt {
public:
    explicit ContinueStmt(SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::ContinueStmt, loc) {}

    void accept(ASTVisitor& visitor) override;
};

// var = expr; or let var = expr;
class VarDeclStmt : public Stmt {
public:
    VarDeclStmt(const std::string& name,
                std::unique_ptr<Expr> init,
                std::optional<Type> type = std::nullopt,
                SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::VarDeclStmt, loc),
          name_(name), init_(std::move(init)), type_(type) {}

    const std::string& getName() const { return name_; }
    Expr* getInit() const { return init_.get(); }
    std::optional<Type> getType() const { return type_; }

    void accept(ASTVisitor& visitor) override;

private:
    std::string name_;
    std::unique_ptr<Expr> init_;
    std::optional<Type> type_;
};

// import {name1, name2 as alias} from "./module.naab"
// import * as mod from "./module.naab"
class ImportStmt : public Stmt {
public:
    struct ImportItem {
        std::string name;
        std::string alias;  // Empty if no alias

        ImportItem(std::string n, std::string a = "")
            : name(std::move(n)), alias(std::move(a)) {}
    };

    ImportStmt(std::vector<ImportItem> items,
               std::string module_path,
               bool is_wildcard = false,
               std::string wildcard_alias = "",
               SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::ImportStmt, loc),
          items_(std::move(items)), module_path_(std::move(module_path)),
          is_wildcard_(is_wildcard), wildcard_alias_(std::move(wildcard_alias)) {}

    const std::vector<ImportItem>& getItems() const { return items_; }
    const std::string& getModulePath() const { return module_path_; }
    bool isWildcard() const { return is_wildcard_; }
    const std::string& getWildcardAlias() const { return wildcard_alias_; }

    void accept(ASTVisitor& visitor) override;

private:
    std::vector<ImportItem> items_;  // Named imports
    std::string module_path_;        // Module file path
    bool is_wildcard_;               // true for "import * as mod"
    std::string wildcard_alias_;     // Alias for wildcard import
};

// export function foo() { ... }
// export var x = 10;
class ExportStmt : public Stmt {
public:
    enum class ExportKind {
        Function,      // export function foo() { ... }
        Variable,      // export var x = 10
        DefaultExpr,   // export default expr
        Struct,        // export struct Point { ... } (Week 7)
        Enum           // export enum LogLevel { ... } (Phase 4.1: Module System)
    };

    // For exporting function
    ExportStmt(std::unique_ptr<FunctionDecl> func,
               SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::ExportStmt, loc),
          kind_(ExportKind::Function),
          function_(std::move(func)) {}

    // For exporting variable
    ExportStmt(std::unique_ptr<VarDeclStmt> var,
               SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::ExportStmt, loc),
          kind_(ExportKind::Variable),
          variable_(std::move(var)) {}

    // For exporting struct (Week 7)
    ExportStmt(std::unique_ptr<StructDecl> struct_decl,
               SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::ExportStmt, loc),
          kind_(ExportKind::Struct),
          struct_decl_(std::move(struct_decl)) {}

    // For exporting enum (Phase 4.1: Module System)
    ExportStmt(std::unique_ptr<EnumDecl> enum_decl,
               SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::ExportStmt, loc),
          kind_(ExportKind::Enum),
          enum_decl_(std::move(enum_decl)) {}

    // For default export (public constructor needed for make_unique)
    explicit ExportStmt(SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::ExportStmt, loc), kind_(ExportKind::DefaultExpr) {}

    // Helper to create default export
    static std::unique_ptr<ExportStmt> createDefault(
        std::unique_ptr<Expr> expr,
        SourceLocation loc = SourceLocation()) {
        auto stmt = std::make_unique<ExportStmt>(loc);
        stmt->default_expr_ = std::move(expr);
        return stmt;
    }

    ExportKind getKind() const { return kind_; }
    FunctionDecl* getFunctionDecl() const { return function_.get(); }
    VarDeclStmt* getVarDecl() const { return variable_.get(); }
    Expr* getExpr() const { return default_expr_.get(); }
    StructDecl* getStructDecl() const { return struct_decl_.get(); }  // Week 7
    EnumDecl* getEnumDecl() const { return enum_decl_.get(); }  // Phase 4.1

    void accept(ASTVisitor& visitor) override;

private:

    ExportKind kind_;
    std::unique_ptr<FunctionDecl> function_;
    std::unique_ptr<VarDeclStmt> variable_;
    std::unique_ptr<Expr> default_expr_;
    std::unique_ptr<StructDecl> struct_decl_;  // Week 7
    std::unique_ptr<EnumDecl> enum_decl_;  // Phase 4.1: Module System
};

// Phase 4.1: try { ... } catch (e) { ... } finally { ... }
class TryStmt : public Stmt {
public:
    // Catch clause: catch (errorName) { body }
    struct CatchClause {
        std::string error_name;  // Parameter name (e.g., "e" in catch(e))
        std::unique_ptr<CompoundStmt> body;

        CatchClause(std::string name, std::unique_ptr<CompoundStmt> b)
            : error_name(std::move(name)), body(std::move(b)) {}
    };

    TryStmt(std::unique_ptr<CompoundStmt> try_body,
            std::unique_ptr<CatchClause> catch_clause,
            std::unique_ptr<CompoundStmt> finally_body = nullptr,
            SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::TryStmt, loc),
          try_body_(std::move(try_body)),
          catch_clause_(std::move(catch_clause)),
          finally_body_(std::move(finally_body)) {}

    CompoundStmt* getTryBody() const { return try_body_.get(); }
    CatchClause* getCatchClause() const { return catch_clause_.get(); }
    CompoundStmt* getFinallyBody() const { return finally_body_.get(); }
    bool hasFinally() const { return finally_body_ != nullptr; }

    void accept(ASTVisitor& visitor) override;

private:
    std::unique_ptr<CompoundStmt> try_body_;
    std::unique_ptr<CatchClause> catch_clause_;
    std::unique_ptr<CompoundStmt> finally_body_;  // Optional
};

// Phase 4.1: throw expression;
class ThrowStmt : public Stmt {
public:
    explicit ThrowStmt(std::unique_ptr<Expr> expr,
                      SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::ThrowStmt, loc), expr_(std::move(expr)) {}

    Expr* getExpr() const { return expr_.get(); }

    void accept(ASTVisitor& visitor) override;

private:
    std::unique_ptr<Expr> expr_;
};

// Phase 4.0: use module_name
// Simple module import statement for Rust-style module system
class ModuleUseStmt : public Stmt {
public:
    explicit ModuleUseStmt(const std::string& module_path,
                          const std::string& alias = "",
                          SourceLocation loc = SourceLocation())
        : Stmt(NodeKind::ModuleUseStmt, loc),
          module_path_(module_path),
          alias_(alias) {}

    const std::string& getModulePath() const { return module_path_; }
    const std::string& getAlias() const { return alias_; }
    bool hasAlias() const { return !alias_.empty(); }

    void accept(ASTVisitor& visitor) override;

private:
    std::string module_path_;  // "math_utils" or "data.processor"
    std::string alias_;        // Optional alias (e.g., "use data.processor as dp")
};

// ============================================================================
// Expressions
// ============================================================================

class Expr : public ASTNode {
public:
    explicit Expr(NodeKind kind, SourceLocation loc = SourceLocation())
        : ASTNode(kind, loc), cached_type_(nullptr) {}

    virtual Type getType() const = 0;

    // Type caching for static analysis (Phase 4)
    void setCachedType(std::shared_ptr<void> type) const {
        cached_type_ = type;
    }

    std::shared_ptr<void> getCachedType() const {
        return cached_type_;
    }

private:
    mutable std::shared_ptr<void> cached_type_;  // Type from TypeChecker (typecheck::Type)
};

// Binary operators: +, -, *, /, ==, !=, <, >, etc.
enum class BinaryOp {
    Add, Sub, Mul, Div, Mod,
    Eq, Ne, Lt, Le, Gt, Ge,
    And, Or,
    Assign,
    Pipeline,  // |>
    Subscript,  // []
};

class BinaryExpr : public Expr {
public:
    BinaryExpr(BinaryOp op,
               std::unique_ptr<Expr> left,
               std::unique_ptr<Expr> right,
               SourceLocation loc = SourceLocation())
        : Expr(NodeKind::BinaryExpr, loc),
          op_(op), left_(std::move(left)), right_(std::move(right)) {}

    BinaryOp getOp() const { return op_; }
    Expr* getLeft() const { return left_.get(); }
    Expr* getRight() const { return right_.get(); }

    Type getType() const override;
    void accept(ASTVisitor& visitor) override;

private:
    BinaryOp op_;
    std::unique_ptr<Expr> left_;
    std::unique_ptr<Expr> right_;
};

// Unary operators: !, -, +
enum class UnaryOp {
    Not, Neg, Pos,
};

class UnaryExpr : public Expr {
public:
    UnaryExpr(UnaryOp op,
              std::unique_ptr<Expr> operand,
              SourceLocation loc = SourceLocation())
        : Expr(NodeKind::UnaryExpr, loc),
          op_(op), operand_(std::move(operand)) {}

    UnaryOp getOp() const { return op_; }
    Expr* getOperand() const { return operand_.get(); }

    Type getType() const override;
    void accept(ASTVisitor& visitor) override;

private:
    UnaryOp op_;
    std::unique_ptr<Expr> operand_;
};

// Function call: func(arg1, arg2, ...)
class CallExpr : public Expr {
public:
    CallExpr(std::unique_ptr<Expr> callee,
             std::vector<std::unique_ptr<Expr>> args,
             std::vector<Type> type_args = {},
             SourceLocation loc = SourceLocation())
        : Expr(NodeKind::CallExpr, loc),
          callee_(std::move(callee)),
          args_(std::move(args)),
          type_arguments_(std::move(type_args)) {}

    Expr* getCallee() const { return callee_.get(); }
    const std::vector<std::unique_ptr<Expr>>& getArgs() const {
        return args_;
    }
    const std::vector<Type>& getTypeArguments() const {
        return type_arguments_;
    }

    Type getType() const override;
    void accept(ASTVisitor& visitor) override;

private:
    std::unique_ptr<Expr> callee_;
    std::vector<std::unique_ptr<Expr>> args_;
    std::vector<Type> type_arguments_;  // Phase 2.4.4: Explicit type arguments for generics
};

// Member access: obj.member
class MemberExpr : public Expr {
public:
    MemberExpr(std::unique_ptr<Expr> obj,
               const std::string& member,
               SourceLocation loc = SourceLocation())
        : Expr(NodeKind::MemberExpr, loc),
          obj_(std::move(obj)), member_(member) {}

    Expr* getObject() const { return obj_.get(); }
    const std::string& getMember() const { return member_; }

    Type getType() const override;
    void accept(ASTVisitor& visitor) override;

private:
    std::unique_ptr<Expr> obj_;
    std::string member_;
};

// Variable reference: x
class IdentifierExpr : public Expr {
public:
    explicit IdentifierExpr(const std::string& name,
                           SourceLocation loc = SourceLocation())
        : Expr(NodeKind::IdentifierExpr, loc), name_(name) {}

    const std::string& getName() const { return name_; }

    Type getType() const override;
    void accept(ASTVisitor& visitor) override;

private:
    std::string name_;
};

// Literals: 42, 3.14, "hello", true
enum class LiteralKind {
    Int, Float, String, Bool, Null
};

class LiteralExpr : public Expr {
public:
    LiteralExpr(LiteralKind kind, const std::string& value,
                SourceLocation loc = SourceLocation())
        : Expr(NodeKind::LiteralExpr, loc), kind_(kind), value_(value) {}

    LiteralKind getLiteralKind() const { return kind_; }
    const std::string& getValue() const { return value_; }

    Type getType() const override;
    void accept(ASTVisitor& visitor) override;

private:
    LiteralKind kind_;
    std::string value_;
};

// Dictionary literal: {key1: val1, key2: val2}
class DictExpr : public Expr {
public:
    using KeyValue = std::pair<std::unique_ptr<Expr>, std::unique_ptr<Expr>>;

    explicit DictExpr(std::vector<KeyValue> entries,
                     SourceLocation loc = SourceLocation())
        : Expr(NodeKind::DictExpr, loc), entries_(std::move(entries)) {}

    const std::vector<KeyValue>& getEntries() const { return entries_; }

    Type getType() const override;
    void accept(ASTVisitor& visitor) override;

private:
    std::vector<KeyValue> entries_;
};

// List literal: [elem1, elem2, ...]
class ListExpr : public Expr {
public:
    explicit ListExpr(std::vector<std::unique_ptr<Expr>> elements,
                     SourceLocation loc = SourceLocation())
        : Expr(NodeKind::ListExpr, loc), elements_(std::move(elements)) {}

    const std::vector<std::unique_ptr<Expr>>& getElements() const {
        return elements_;
    }

    Type getType() const override;
    void accept(ASTVisitor& visitor) override;

private:
    std::vector<std::unique_ptr<Expr>> elements_;
};

// Range operator: start..end (exclusive) or start..=end (inclusive)
class RangeExpr : public Expr {
public:
    RangeExpr(std::unique_ptr<Expr> start,
              std::unique_ptr<Expr> end,
              bool inclusive = false,
              SourceLocation loc = SourceLocation())
        : Expr(NodeKind::RangeExpr, loc),
          start_(std::move(start)), end_(std::move(end)), inclusive_(inclusive) {}

    Expr* getStart() const { return start_.get(); }
    Expr* getEnd() const { return end_.get(); }
    bool isInclusive() const { return inclusive_; }

    Type getType() const override;
    void accept(ASTVisitor& visitor) override;

private:
    std::unique_ptr<Expr> start_;
    std::unique_ptr<Expr> end_;
    bool inclusive_;
};

class StructLiteralExpr : public Expr {
public:
    StructLiteralExpr(std::string name,
                      std::vector<std::pair<std::string, std::unique_ptr<Expr>>> inits,
                      SourceLocation loc)
        : Expr(NodeKind::StructLiteralExpr, loc),
          struct_name_(std::move(name)), field_inits_(std::move(inits)) {}

    const std::string& getStructName() const { return struct_name_; }
    const std::vector<std::pair<std::string, std::unique_ptr<Expr>>>&
        getFieldInits() const { return field_inits_; }

    Type getType() const override { return Type::makeStruct(struct_name_); }
    void accept(ASTVisitor& visitor) override;

private:
    std::string struct_name_;
    std::vector<std::pair<std::string, std::unique_ptr<Expr>>> field_inits_;
};

// Inline polyglot code: <<language code >> or <<language[var1, var2] code >>
class InlineCodeExpr : public Expr {
public:
    InlineCodeExpr(std::string language, std::string code,
                   std::vector<std::string> bound_variables = {},
                   SourceLocation loc = SourceLocation())
        : Expr(NodeKind::InlineCodeExpr, loc),
          language_(std::move(language)), code_(std::move(code)),
          bound_variables_(std::move(bound_variables)) {}

    const std::string& getLanguage() const { return language_; }
    const std::string& getCode() const { return code_; }
    const std::vector<std::string>& getBoundVariables() const { return bound_variables_; }

    Type getType() const override { return Type::makeVoid(); }
    void accept(ASTVisitor& visitor) override;

private:
    std::string language_;
    std::string code_;
    std::vector<std::string> bound_variables_;  // Phase 2.2: Variables to bind from NAAb scope
};

// ============================================================================
// Program (top-level)
// ============================================================================

class Program : public ASTNode {
public:
    Program(std::vector<std::unique_ptr<UseStatement>> imports,
            std::vector<std::unique_ptr<FunctionDecl>> functions,
            std::unique_ptr<MainBlock> main_block,
            SourceLocation loc = SourceLocation())
        : ASTNode(NodeKind::Program, loc),
          imports_(std::move(imports)),
          functions_(std::move(functions)),
          main_block_(std::move(main_block)) {}

    const std::vector<std::unique_ptr<UseStatement>>& getImports() const {
        return imports_;
    }
    const std::vector<std::unique_ptr<ImportStmt>>& getModuleImports() const {
        return module_imports_;
    }
    const std::vector<std::unique_ptr<ModuleUseStmt>>& getModuleUses() const {  // Phase 4.0
        return module_uses_;
    }
    const std::vector<std::unique_ptr<ExportStmt>>& getExports() const {
        return exports_;
    }
    const std::vector<std::unique_ptr<FunctionDecl>>& getFunctions() const {
        return functions_;
    }
    const std::vector<std::unique_ptr<StructDecl>>& getStructs() const {
        return structs_;
    }
    const std::vector<std::unique_ptr<EnumDecl>>& getEnums() const {  // Phase 2.4.3
        return enums_;
    }
    MainBlock* getMainBlock() const { return main_block_.get(); }

    // Phase 3.1: Add module imports and exports
    void addModuleImport(std::unique_ptr<ImportStmt> import) {
        module_imports_.push_back(std::move(import));
    }
    void addModuleUse(std::unique_ptr<ModuleUseStmt> module_use) {  // Phase 4.0
        module_uses_.push_back(std::move(module_use));
    }
    void addExport(std::unique_ptr<ExportStmt> export_stmt) {
        exports_.push_back(std::move(export_stmt));
    }
    void addStruct(std::unique_ptr<StructDecl> struct_decl) {
        structs_.push_back(std::move(struct_decl));
    }
    void addEnum(std::unique_ptr<EnumDecl> enum_decl) {  // Phase 2.4.3
        enums_.push_back(std::move(enum_decl));
    }

    void accept(ASTVisitor& visitor) override;

private:
    std::vector<std::unique_ptr<UseStatement>> imports_;  // Legacy block imports
    std::vector<std::unique_ptr<ImportStmt>> module_imports_;  // Phase 3.1: Module imports (ES6-style)
    std::vector<std::unique_ptr<ModuleUseStmt>> module_uses_;  // Phase 4.0: Module uses (Rust-style)
    std::vector<std::unique_ptr<ExportStmt>> exports_;  // Phase 3.1: Module exports
    std::vector<std::unique_ptr<FunctionDecl>> functions_;
    std::vector<std::unique_ptr<StructDecl>> structs_;  // Struct declarations
    std::vector<std::unique_ptr<EnumDecl>> enums_;  // Phase 2.4.3: Enum declarations
    std::unique_ptr<MainBlock> main_block_;
};

// ============================================================================
// Visitor Pattern (for AST traversal)
// ============================================================================

class ASTVisitor {
public:
    virtual ~ASTVisitor() = default;

    virtual void visit(Program& node) = 0;
    virtual void visit(UseStatement& node) = 0;
    virtual void visit(FunctionDecl& node) = 0;
    virtual void visit(MainBlock& node) = 0;

    virtual void visit(CompoundStmt& node) = 0;
    virtual void visit(ExprStmt& node) = 0;
    virtual void visit(ReturnStmt& node) = 0;
    virtual void visit(IfStmt& node) = 0;
    virtual void visit(ForStmt& node) = 0;
    virtual void visit(WhileStmt& node) = 0;
    virtual void visit(BreakStmt& node) = 0;
    virtual void visit(ContinueStmt& node) = 0;
    virtual void visit(VarDeclStmt& node) = 0;
    virtual void visit(ImportStmt& node) = 0;   // Phase 3.1
    virtual void visit(ExportStmt& node) = 0;   // Phase 3.1
    virtual void visit(TryStmt& node) = 0;      // Phase 4.1
    virtual void visit(ThrowStmt& node) = 0;    // Phase 4.1
    virtual void visit(ModuleUseStmt& node) = 0;  // Phase 4.0

    virtual void visit(BinaryExpr& node) = 0;
    virtual void visit(UnaryExpr& node) = 0;
    virtual void visit(CallExpr& node) = 0;
    virtual void visit(MemberExpr& node) = 0;
    virtual void visit(IdentifierExpr& node) = 0;
    virtual void visit(LiteralExpr& node) = 0;
    virtual void visit(DictExpr& node) = 0;
    virtual void visit(ListExpr& node) = 0;

    // Range operator (default implementation - non-breaking)
    virtual void visit(RangeExpr& node) {
        (void)node; // Mark as intentionally unused
        throw std::runtime_error("RangeExpr not supported by this visitor");
    }

    // Struct support (default implementations - non-breaking)
    virtual void visit(StructDecl& node) {
        (void)node; // Mark as intentionally unused
        throw std::runtime_error("StructDecl not supported by this visitor");
    }
    virtual void visit(StructLiteralExpr& node) {
        (void)node; // Mark as intentionally unused
        throw std::runtime_error("StructLiteralExpr not supported by this visitor");
    }
    virtual void visit(InlineCodeExpr& node) {
        (void)node; // Mark as intentionally unused
        throw std::runtime_error("InlineCodeExpr not supported by this visitor");
    }
    // Phase 2.4.3: Enum support
    virtual void visit(EnumDecl& node) {
        (void)node; // Mark as intentionally unused
        throw std::runtime_error("EnumDecl not supported by this visitor");
    }
};

} // namespace ast
} // namespace naab

#endif // NAAB_AST_H
