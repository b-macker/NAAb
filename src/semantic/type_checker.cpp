// NAAb Type Checker Implementation - Stub
// Provides basic type validation framework

#include "naab/type_checker.h"
#include <fmt/core.h>
#include <sstream>

namespace naab {
namespace typecheck {

// ============================================================================
// Type Implementation
// ============================================================================

std::string Type::toString() const {
    switch (kind) {
        case TypeKind::Void: return "void";
        case TypeKind::Int: return "int";
        case TypeKind::Float: return "float";
        case TypeKind::Bool: return "bool";
        case TypeKind::String: return "string";
        case TypeKind::List: return "list";
        case TypeKind::Dict: return "dict";
        case TypeKind::Function: return "function";
        case TypeKind::Block: return "block";
        case TypeKind::PythonObject: return "python_object";
        case TypeKind::Any: return "any";
        case TypeKind::Unknown: return "unknown";
    }
    return "?";
}

bool Type::isCompatibleWith(const Type& other) const {
    if (kind == TypeKind::Any || other.kind == TypeKind::Any) return true;
    if (kind == TypeKind::Unknown || other.kind == TypeKind::Unknown) return true;
    if (kind == other.kind) return true;
    if (kind == TypeKind::Int && other.kind == TypeKind::Float) return true;
    return false;
}

bool Type::isNumeric() const {
    return kind == TypeKind::Int || kind == TypeKind::Float;
}

// Factory methods
std::shared_ptr<Type> Type::makeVoid() { return std::make_shared<Type>(TypeKind::Void); }
std::shared_ptr<Type> Type::makeInt() { return std::make_shared<Type>(TypeKind::Int); }
std::shared_ptr<Type> Type::makeFloat() { return std::make_shared<Type>(TypeKind::Float); }
std::shared_ptr<Type> Type::makeBool() { return std::make_shared<Type>(TypeKind::Bool); }
std::shared_ptr<Type> Type::makeString() { return std::make_shared<Type>(TypeKind::String); }
std::shared_ptr<Type> Type::makeList(std::shared_ptr<Type>) {
    return std::make_shared<Type>(TypeKind::List);
}
std::shared_ptr<Type> Type::makeDict(std::shared_ptr<Type>, std::shared_ptr<Type>) {
    return std::make_shared<Type>(TypeKind::Dict);
}
std::shared_ptr<Type> Type::makeFunction(std::vector<std::shared_ptr<Type>>, std::shared_ptr<Type>) {
    return std::make_shared<Type>(TypeKind::Function);
}
std::shared_ptr<Type> Type::makeBlock() { return std::make_shared<Type>(TypeKind::Block); }
std::shared_ptr<Type> Type::makePythonObject() { return std::make_shared<Type>(TypeKind::PythonObject); }
std::shared_ptr<Type> Type::makeAny() { return std::make_shared<Type>(TypeKind::Any); }
std::shared_ptr<Type> Type::makeUnknown() { return std::make_shared<Type>(TypeKind::Unknown); }

// ============================================================================
// TypeError Implementation
// ============================================================================

std::string TypeError::toString() const {
    std::ostringstream ss;
    ss << "[Type Error] Line " << line << ":" << column << ": " << message;
    return ss.str();
}

// ============================================================================
// TypeEnvironment Implementation
// ============================================================================

void TypeEnvironment::define(const std::string& name, std::shared_ptr<Type> type) {
    types_[name] = type;
}

std::shared_ptr<Type> TypeEnvironment::get(const std::string& name) {
    auto it = types_.find(name);
    if (it != types_.end()) return it->second;
    if (parent_) return parent_->get(name);
    return nullptr;
}

void TypeEnvironment::set(const std::string& name, std::shared_ptr<Type> type) {
    types_[name] = type;
}

bool TypeEnvironment::has(const std::string& name) const {
    if (types_.find(name) != types_.end()) return true;
    if (parent_) return parent_->has(name);
    return false;
}

// ============================================================================
// TypeChecker Implementation - Stub
// ============================================================================

TypeChecker::TypeChecker()
    : env_(std::make_shared<TypeEnvironment>()),
      current_type_(Type::makeVoid()),
      current_function_return_type_(nullptr) {
}

std::vector<TypeError> TypeChecker::check(std::shared_ptr<ast::Program> program) {
    errors_.clear();
    if (program) {
        program->accept(*this);
    }
    return errors_;
}

// Stub implementations - accept nodes but don't check yet
void TypeChecker::visit(ast::Program& node) {
    // Visit all top-level declarations
    for (const auto& func : node.getFunctions()) {
        func->accept(*this);
    }
    for (const auto& exp : node.getExports()) {
        exp->accept(*this);
    }
    if (node.getMainBlock()) {
        node.getMainBlock()->accept(*this);
    }
    current_type_ = Type::makeVoid();
}

void TypeChecker::visit(ast::UseStatement&) { current_type_ = Type::makeVoid(); }
void TypeChecker::visit(ast::FunctionDecl& node) {
    auto loc = node.getLocation();

    // Check if async (not yet supported - deferred to Phase 6/v2.0)
    if (node.isAsync()) {
        reportError(
            "Native async/await not yet implemented. Use polyglot async execution instead:\n"
            "  Example: let result = <<python import asyncio; asyncio.run(my_async_func()) >>",
            loc.line, loc.column
        );
    }

    // Phase 2: Build proper function type from parameters
    std::vector<std::shared_ptr<Type>> param_types;
    for (const auto& param : node.getParams()) {
        param_types.push_back(convertAstType(param.type));
    }
    auto return_type = convertAstType(node.getReturnType());
    auto func_type = Type::makeFunction(param_types, return_type);

    // Add function to environment
    env_->define(node.getName(), func_type);

    // Add function to symbol table
    semantic::Symbol func_symbol(
        node.getName(),
        semantic::SymbolKind::Function,
        func_type->toString(),  // Phase 2: Use actual function type
        semantic::SourceLocation(current_filename_, loc.line, loc.column)
    );
    symbol_table_.define(node.getName(), std::move(func_symbol));

    // Create new scope for function body
    pushScope();
    symbol_table_.push_scope();

    // Add parameters to new scope
    for (const auto& param : node.getParams()) {
        // Phase 2: Convert ast::Type to typecheck::Type properly
        auto param_type = convertAstType(param.type);
        env_->define(param.name, param_type);

        // Add parameter to symbol table
        semantic::Symbol param_symbol(
            param.name,
            semantic::SymbolKind::Parameter,
            param_type->toString(),  // Phase 2: Get actual parameter type
            semantic::SourceLocation(current_filename_, loc.line, loc.column)
        );
        symbol_table_.define(param.name, std::move(param_symbol));
    }

    // Set current function return type for return statement checking
    current_function_return_type_ = return_type;  // Phase 2: Use actual return type

    // Type check function body
    if (node.getBody()) {
        node.getBody()->accept(*this);
    }

    // Restore state
    current_function_return_type_ = nullptr;
    symbol_table_.pop_scope();
    popScope();

    current_type_ = Type::makeVoid();
}
void TypeChecker::visit(ast::MainBlock& node) {
    pushScope();
    if (node.getBody()) { node.getBody()->accept(*this); }
    popScope();
    current_type_ = Type::makeVoid();
}

void TypeChecker::visit(ast::CompoundStmt& node) {
    for (const auto& stmt : node.getStatements()) { stmt->accept(*this); }
    current_type_ = Type::makeVoid();
}

void TypeChecker::visit(ast::ExprStmt& node) {
    if (node.getExpr()) { node.getExpr()->accept(*this); }
    current_type_ = Type::makeVoid();
}
void TypeChecker::visit(ast::ReturnStmt& node) {
    auto loc = node.getLocation();

    // Infer return value type
    std::shared_ptr<Type> return_type = Type::makeVoid();
    if (node.getExpr()) {
        node.getExpr()->accept(*this);
        return_type = current_type_;
    }

    // Check against function's declared return type
    if (current_function_return_type_) {
        if (!checkTypeCompatibility(return_type, current_function_return_type_,
                                   "Return statement", loc.line, loc.column)) {
            reportError(
                fmt::format("Return type mismatch: expected {}, got {}",
                           current_function_return_type_->toString(),
                           return_type->toString()),
                loc.line, loc.column
            );
        }
    }

    current_type_ = Type::makeVoid();
}
void TypeChecker::visit(ast::IfStmt& node) {
    if (node.getCondition()) { node.getCondition()->accept(*this); }
    pushScope();
    symbol_table_.push_scope();
    if (node.getThenBranch()) { node.getThenBranch()->accept(*this); }
    symbol_table_.pop_scope();
    popScope();
    if (node.getElseBranch()) {
        pushScope();
        symbol_table_.push_scope();
        node.getElseBranch()->accept(*this);
        symbol_table_.pop_scope();
        popScope();
    }
    current_type_ = Type::makeVoid();
}

void TypeChecker::visit(ast::ForStmt& node) {
    auto loc = node.getLocation();
    if (node.getIter()) { node.getIter()->accept(*this); }

    // Phase 3: Infer loop variable type from iterable
    auto iterable_type = current_type_;
    std::shared_ptr<Type> loop_var_type = Type::makeAny();
    if (iterable_type && iterable_type->kind == TypeKind::List && iterable_type->element_type) {
        loop_var_type = iterable_type->element_type;
    }

    pushScope();
    symbol_table_.push_scope();
    env_->define(node.getVar(), loop_var_type);

    // Add loop variable to symbol table
    semantic::Symbol loop_var_symbol(
        node.getVar(),
        semantic::SymbolKind::Variable,
        loop_var_type->toString(),  // Phase 3: Use inferred type
        semantic::SourceLocation(current_filename_, loc.line, loc.column)
    );
    symbol_table_.define(node.getVar(), std::move(loop_var_symbol));

    if (node.getBody()) { node.getBody()->accept(*this); }
    symbol_table_.pop_scope();
    popScope();
    current_type_ = Type::makeVoid();
}

void TypeChecker::visit(ast::WhileStmt& node) {
    if (node.getCondition()) { node.getCondition()->accept(*this); }
    pushScope();
    symbol_table_.push_scope();
    if (node.getBody()) { node.getBody()->accept(*this); }
    symbol_table_.pop_scope();
    popScope();
    current_type_ = Type::makeVoid();
}
void TypeChecker::visit(ast::BreakStmt&) { current_type_ = Type::makeVoid(); }
void TypeChecker::visit(ast::ContinueStmt&) { current_type_ = Type::makeVoid(); }
void TypeChecker::visit(ast::VarDeclStmt& node) {
    auto loc = node.getLocation();

    // Step 1: Infer initializer type
    std::shared_ptr<Type> init_type = Type::makeAny();
    if (node.getInit()) {
        node.getInit()->accept(*this);
        init_type = current_type_;
    }

    // Step 2: Get declared type if present
    std::shared_ptr<Type> decl_type = Type::makeAny();
    if (node.getType().has_value()) {
        // Type is already a Type object, convert to shared_ptr
        const auto& type_val = node.getType().value();
        // TODO: Convert ast::Type to typecheck::Type properly
        // For now, we'll use the init type
        decl_type = init_type;
    }

    // Step 3: Check compatibility
    if (node.getType().has_value() && node.getInit()) {
        if (!checkTypeCompatibility(init_type, decl_type,
                                   "Variable initialization",
                                   loc.line, loc.column)) {
            reportError(
                fmt::format("Type mismatch: Cannot assign {} to variable of type {}",
                           init_type->toString(), decl_type->toString()),
                loc.line, loc.column
            );
        }
    }

    // Step 4: Add to type environment
    std::shared_ptr<Type> final_type =
        (decl_type->kind != TypeKind::Any) ? decl_type : init_type;
    env_->define(node.getName(), final_type);

    // Step 5: Add to symbol table for LSP support
    semantic::Symbol symbol(
        node.getName(),
        semantic::SymbolKind::Variable,
        final_type->toString(),
        semantic::SourceLocation(current_filename_, loc.line, loc.column)
    );
    symbol_table_.define(node.getName(), std::move(symbol));

    current_type_ = Type::makeVoid();
}
void TypeChecker::visit(ast::ImportStmt&) { current_type_ = Type::makeVoid(); }

void TypeChecker::visit(ast::ExportStmt& node) {
    // Visit the exported declaration based on kind
    switch (node.getKind()) {
        case ast::ExportStmt::ExportKind::Function:
            if (node.getFunctionDecl()) node.getFunctionDecl()->accept(*this);
            break;
        case ast::ExportStmt::ExportKind::Variable:
            if (node.getVarDecl()) node.getVarDecl()->accept(*this);
            break;
        case ast::ExportStmt::ExportKind::DefaultExpr:
            if (node.getExpr()) node.getExpr()->accept(*this);
            break;
        default:
            break;
    }
    current_type_ = Type::makeVoid();
}

void TypeChecker::visit(ast::ModuleUseStmt&) { current_type_ = Type::makeVoid(); }

void TypeChecker::visit(ast::TryStmt& node) {
    auto loc = node.getLocation();
    pushScope();
    symbol_table_.push_scope();
    if (node.getTryBody()) { node.getTryBody()->accept(*this); }
    symbol_table_.pop_scope();
    popScope();

    // Handle catch clause (singular)
    if (auto* cc = node.getCatchClause()) {
        pushScope();
        symbol_table_.push_scope();
        env_->define(cc->error_name, Type::makeAny());

        // Add exception variable to symbol table
        // Phase 6: Track exception type (currently always Any since catch clauses
        // don't have type annotations - would need syntax like "catch(e: Error)")
        semantic::Symbol exc_var_symbol(
            cc->error_name,
            semantic::SymbolKind::Variable,
            "any",  // Exception type - always Any without type annotations
            semantic::SourceLocation(current_filename_, loc.line, loc.column)
        );
        symbol_table_.define(cc->error_name, std::move(exc_var_symbol));

        if (cc->body) { cc->body->accept(*this); }
        symbol_table_.pop_scope();
        popScope();
    }

    if (node.getFinallyBody()) {
        pushScope();
        symbol_table_.push_scope();
        node.getFinallyBody()->accept(*this);
        symbol_table_.pop_scope();
        popScope();
    }
    current_type_ = Type::makeVoid();
}

void TypeChecker::visit(ast::ThrowStmt& node) {
    if (node.getExpr()) { node.getExpr()->accept(*this); }
    current_type_ = Type::makeVoid();
}

void TypeChecker::visit(ast::FunctionDeclStmt& node) {
    // Delegate to the wrapped FunctionDecl
    node.getDecl()->accept(*this);
}

void TypeChecker::visit(ast::StructDeclStmt& node) {
    // Delegate to the wrapped StructDecl
    node.getDecl()->accept(*this);
}

void TypeChecker::visit(ast::BinaryExpr& node) {
    // Step 1: Infer left operand type
    node.getLeft()->accept(*this);
    auto left_type = current_type_;

    // Step 2: Infer right operand type
    node.getRight()->accept(*this);
    auto right_type = current_type_;

    // Step 3: Convert BinaryOp to string
    std::string op_str;
    switch (node.getOp()) {
        case ast::BinaryOp::Add: op_str = "+"; break;
        case ast::BinaryOp::Sub: op_str = "-"; break;
        case ast::BinaryOp::Mul: op_str = "*"; break;
        case ast::BinaryOp::Div: op_str = "/"; break;
        case ast::BinaryOp::Mod: op_str = "%"; break;
        case ast::BinaryOp::Eq: op_str = "=="; break;
        case ast::BinaryOp::Ne: op_str = "!="; break;
        case ast::BinaryOp::Lt: op_str = "<"; break;
        case ast::BinaryOp::Le: op_str = "<="; break;
        case ast::BinaryOp::Gt: op_str = ">"; break;
        case ast::BinaryOp::Ge: op_str = ">="; break;
        case ast::BinaryOp::And: op_str = "&&"; break;
        case ast::BinaryOp::Or: op_str = "||"; break;
        case ast::BinaryOp::Assign: op_str = "="; break;
        case ast::BinaryOp::Pipeline: op_str = "|>"; break;
        case ast::BinaryOp::Subscript: op_str = "[]"; break;
    }

    // Step 4: Check operator compatibility
    auto loc = node.getLocation();
    current_type_ = inferBinaryOpType(
        op_str,
        left_type,
        right_type,
        loc.line,
        loc.column
    );

    // Step 5: Report errors if incompatible
    if (current_type_->kind == TypeKind::Unknown) {
        reportError(
            fmt::format("Type error: Cannot apply '{}' to {} and {}",
                       op_str, left_type->toString(), right_type->toString()),
            loc.line, loc.column
        );
    }

    // Phase 4: Cache type in AST node
    node.setCachedType(current_type_);
}
void TypeChecker::visit(ast::UnaryExpr& node) {
    node.getOperand()->accept(*this);
    auto operand_type = current_type_;

    // Convert UnaryOp to string
    std::string op_str;
    switch (node.getOp()) {
        case ast::UnaryOp::Neg: op_str = "-"; break;
        case ast::UnaryOp::Not: op_str = "!"; break;
        case ast::UnaryOp::Pos: op_str = "+"; break;
    }

    auto loc = node.getLocation();
    current_type_ = inferUnaryOpType(op_str, operand_type, loc.line, loc.column);

    if (current_type_->kind == TypeKind::Unknown) {
        reportError(
            fmt::format("Type error: Cannot apply '{}' to {}",
                       op_str, operand_type->toString()),
            loc.line, loc.column
        );
    }

    // Phase 4: Cache type in AST node
    node.setCachedType(current_type_);
}
void TypeChecker::visit(ast::CallExpr& node) {
    // Step 1: Infer callee type
    node.getCallee()->accept(*this);
    auto callee_type = current_type_;

    auto loc = node.getLocation();

    // Step 2: Check if it's a function
    if (callee_type->kind != TypeKind::Function && callee_type->kind != TypeKind::Any) {
        reportError(
            fmt::format("Cannot call non-function type: {}", callee_type->toString()),
            loc.line, loc.column
        );
        current_type_ = Type::makeUnknown();
        return;
    }

    // Step 3: Check argument types and validate signature (Phase 7)
    std::vector<std::shared_ptr<Type>> arg_types;
    for (const auto& arg : node.getArgs()) {
        arg->accept(*this);
        arg_types.push_back(current_type_);
    }

    // Phase 7: Match against function signature when available
    if (callee_type->kind == TypeKind::Function) {
        // Check argument count
        size_t expected_count = callee_type->param_types.size();
        size_t actual_count = arg_types.size();

        if (actual_count != expected_count) {
            reportError(
                fmt::format("Function expects {} argument{}, got {}",
                           expected_count, expected_count == 1 ? "" : "s", actual_count),
                loc.line, loc.column
            );
        }

        // Check argument types
        size_t min_count = std::min(actual_count, expected_count);
        for (size_t i = 0; i < min_count; ++i) {
            auto param_type = callee_type->param_types[i];
            auto arg_type = arg_types[i];

            if (!checkTypeCompatibility(param_type, arg_type, "function argument", loc.line, loc.column)) {
                reportError(
                    fmt::format("Argument {} type mismatch: expected {}, got {}",
                               i + 1, param_type->toString(), arg_type->toString()),
                    loc.line, loc.column
                );
            }
        }

        // Return function's return type
        current_type_ = callee_type->return_type;
    } else {
        // For Any type or unknown functions, return Any
        current_type_ = Type::makeAny();
    }

    // Phase 4: Cache type in AST node
    node.setCachedType(current_type_);
}
void TypeChecker::visit(ast::MemberExpr& node) {
    // Type check the object
    node.getObject()->accept(*this);
    auto object_type = current_type_;

    // Phase 5: Member type lookup (deferred - requires struct/class type system)
    // Would need to look up field type from struct definition:
    // if (object_type->kind == TypeKind::Struct) {
    //     current_type_ = lookupStructField(object_type->struct_name, member_name);
    // }
    // For now, assume member access returns Any
    current_type_ = Type::makeAny();

    // Phase 4: Cache type in AST node
    node.setCachedType(current_type_);
}
void TypeChecker::visit(ast::IdentifierExpr& node) {
    // Look up variable in type environment
    auto type = env_->get(node.getName());

    if (!type) {
        auto loc = node.getLocation();
        reportError(
            fmt::format("Undefined variable: '{}'", node.getName()),
            loc.line, loc.column
        );
        current_type_ = Type::makeUnknown();
        node.setCachedType(current_type_);  // Phase 4
        return;
    }

    current_type_ = type;
    // Phase 4: Cache type in AST node
    node.setCachedType(current_type_);
}
void TypeChecker::visit(ast::LiteralExpr& node) {
    switch (node.getLiteralKind()) {
        case ast::LiteralKind::Int:
            current_type_ = Type::makeInt();
            break;
        case ast::LiteralKind::Float:
            current_type_ = Type::makeFloat();
            break;
        case ast::LiteralKind::String:
            current_type_ = Type::makeString();
            break;
        case ast::LiteralKind::Bool:
            current_type_ = Type::makeBool();
            break;
        case ast::LiteralKind::Null:
            current_type_ = Type::makeAny();  // or makeNull() if you add it
            break;
        default:
            current_type_ = Type::makeUnknown();
    }

    // Phase 4: Cache type in AST node
    node.setCachedType(current_type_);
}
void TypeChecker::visit(ast::DictExpr& node) {
    if (node.getEntries().empty()) {
        current_type_ = Type::makeDict(Type::makeAny(), Type::makeAny());
        return;
    }

    // Infer key/value types from first pair
    const auto& first_entry = node.getEntries()[0];
    first_entry.first->accept(*this);
    auto key_type = current_type_;

    first_entry.second->accept(*this);
    auto value_type = current_type_;

    auto loc = node.getLocation();

    // Check all pairs have compatible types
    for (size_t i = 1; i < node.getEntries().size(); ++i) {
        const auto& entry = node.getEntries()[i];

        entry.first->accept(*this);
        if (!current_type_->isCompatibleWith(*key_type)) {
            reportError(
                fmt::format("Dict key type mismatch: expected {}, got {}",
                           key_type->toString(), current_type_->toString()),
                loc.line, loc.column
            );
        }

        entry.second->accept(*this);
        if (!current_type_->isCompatibleWith(*value_type)) {
            reportError(
                fmt::format("Dict value type mismatch: expected {}, got {}",
                           value_type->toString(), current_type_->toString()),
                loc.line, loc.column
            );
        }
    }

    current_type_ = Type::makeDict(key_type, value_type);
    // Phase 4: Cache type in AST node
    node.setCachedType(current_type_);
}
void TypeChecker::visit(ast::ListExpr& node) {
    if (node.getElements().empty()) {
        current_type_ = Type::makeList(Type::makeAny());
        node.setCachedType(current_type_);  // Phase 4
        return;
    }

    // Infer element type from first element
    node.getElements()[0]->accept(*this);
    auto element_type = current_type_;

    auto loc = node.getLocation();

    // Check all elements have compatible types
    for (size_t i = 1; i < node.getElements().size(); ++i) {
        node.getElements()[i]->accept(*this);
        if (!current_type_->isCompatibleWith(*element_type)) {
            reportError(
                fmt::format("List element type mismatch: expected {}, got {}",
                           element_type->toString(), current_type_->toString()),
                loc.line, loc.column
            );
        }
    }

    current_type_ = Type::makeList(element_type);
    // Phase 4: Cache type in AST node
    node.setCachedType(current_type_);
}

// Helper methods
std::shared_ptr<Type> TypeChecker::inferBinaryOpType(
    const std::string& op,
    std::shared_ptr<Type> left,
    std::shared_ptr<Type> right,
    size_t line, size_t column) {

    // Arithmetic operators: +, -, *, /, %
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
        if (left->isNumeric() && right->isNumeric()) {
            // int + int = int, float + float = float, int + float = float
            if (left->kind == TypeKind::Float || right->kind == TypeKind::Float) {
                return Type::makeFloat();
            }
            return Type::makeInt();
        }

        // String concatenation: string + string = string
        if (op == "+" && left->kind == TypeKind::String && right->kind == TypeKind::String) {
            return Type::makeString();
        }

        // List concatenation: list + list = list
        if (op == "+" && left->kind == TypeKind::List && right->kind == TypeKind::List) {
            // Phase 3: Preserve element type if both lists have same element type
            if (left->element_type && right->element_type &&
                left->element_type->isCompatibleWith(*right->element_type)) {
                return Type::makeList(left->element_type);
            }
            return Type::makeList(Type::makeAny());
        }

        return Type::makeUnknown();
    }

    // Comparison operators: ==, !=, <, >, <=, >=
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
        if (left->isCompatibleWith(*right)) {
            return Type::makeBool();
        }
        return Type::makeUnknown();
    }

    // Logical operators: &&, ||
    if (op == "&&" || op == "||") {
        // Both operands should be bool (but allow truthy/falsy conversion)
        return Type::makeBool();
    }

    // Assignment operator
    if (op == "=") {
        // Assignment returns the value type
        return right;
    }

    // Subscript operator: list[int] or dict[key]
    if (op == "[]") {
        if (left->kind == TypeKind::List) {
            // Phase 3: Return actual element type
            if (left->element_type) {
                return left->element_type;
            }
            return Type::makeAny();
        }
        if (left->kind == TypeKind::Dict) {
            // Phase 3: Return actual value type
            if (left->value_type) {
                return left->value_type;
            }
            return Type::makeAny();
        }
        if (left->kind == TypeKind::String) {
            return Type::makeString();  // String indexing returns string
        }
        return Type::makeUnknown();
    }

    // Pipeline operator: a |> b (Phase 5)
    if (op == "|>") {
        // The left side is piped as the first argument to the right side (function)
        if (right->kind == TypeKind::Function) {
            // Validate that left type is compatible with first parameter
            if (!right->param_types.empty()) {
                auto first_param = right->param_types[0];
                if (!checkTypeCompatibility(first_param, left, "pipeline argument", line, column)) {
                    // Type mismatch warning (don't fail, just report)
                    // Pipeline is flexible and runtime will handle conversion if needed
                }
            }
            // Return the function's return type
            return right->return_type ? right->return_type : Type::makeAny();
        }
        // If right side is not a function, return Any (error reported elsewhere)
        return Type::makeAny();
    }

    return Type::makeUnknown();
}

std::shared_ptr<Type> TypeChecker::inferUnaryOpType(
    const std::string& op,
    std::shared_ptr<Type> operand,
    size_t line, size_t column) {

    // Negation: -expr
    if (op == "-") {
        if (operand->isNumeric()) {
            return operand;  // Preserve int or float
        }
        return Type::makeUnknown();
    }

    // Logical NOT: !expr
    if (op == "!") {
        // Allow any type (truthy/falsy conversion)
        return Type::makeBool();
    }

    // Bitwise NOT: ~expr
    if (op == "~") {
        if (operand->kind == TypeKind::Int) {
            return Type::makeInt();
        }
        return Type::makeUnknown();
    }

    return Type::makeUnknown();
}

bool TypeChecker::checkTypeCompatibility(
    std::shared_ptr<Type> actual,
    std::shared_ptr<Type> expected,
    const std::string& context,
    size_t line, size_t column) {

    // Any is compatible with everything
    if (expected->kind == TypeKind::Any || actual->kind == TypeKind::Any) {
        return true;
    }

    // Use existing compatibility check
    if (actual->isCompatibleWith(*expected)) {
        return true;
    }

    // Not compatible - error already reported by caller
    return false;
}

void TypeChecker::reportError(const std::string& message, size_t line, size_t column) {
    errors_.emplace_back(message, line, column);
}

void TypeChecker::pushScope() {
    env_ = std::make_shared<TypeEnvironment>(env_);
}

void TypeChecker::popScope() {
    if (env_ && env_->getParent()) {
        env_ = env_->getParent();
    }
}

// Helper to parse type annotations
std::shared_ptr<Type> TypeChecker::parseTypeAnnotation(const std::string& annotation) {
    if (annotation.empty()) return Type::makeAny();
    if (annotation == "int") return Type::makeInt();
    if (annotation == "float") return Type::makeFloat();
    if (annotation == "bool") return Type::makeBool();
    if (annotation == "string") return Type::makeString();
    if (annotation == "void") return Type::makeVoid();

    // TODO: Parse list<T>, dict<K,V>, function types
    // For now, treat unknown types as Any
    return Type::makeAny();
}

// Helper to convert AST types to TypeChecker types (Phase 1)
std::shared_ptr<Type> TypeChecker::convertAstType(const ast::Type& ast_type) {
    switch (ast_type.kind) {
        case ast::TypeKind::Void:
            return Type::makeVoid();
        case ast::TypeKind::Int:
            return Type::makeInt();
        case ast::TypeKind::Float:
            return Type::makeFloat();
        case ast::TypeKind::String:
            return Type::makeString();
        case ast::TypeKind::Bool:
            return Type::makeBool();
        case ast::TypeKind::List:
            if (ast_type.element_type) {
                return Type::makeList(convertAstType(*ast_type.element_type));
            }
            return Type::makeList(Type::makeAny());
        case ast::TypeKind::Dict:
            if (ast_type.key_value_types) {
                return Type::makeDict(
                    convertAstType(ast_type.key_value_types->first),
                    convertAstType(ast_type.key_value_types->second)
                );
            }
            return Type::makeDict(Type::makeAny(), Type::makeAny());
        case ast::TypeKind::Function:
            // Function types in ast::Type don't store signature details
            // Signature comes from FunctionDecl's getParams() and getReturnType()
            return Type::makeFunction({}, Type::makeAny());
        case ast::TypeKind::Block:
            return Type::makeBlock();
        case ast::TypeKind::Any:
        default:
            return Type::makeAny();
    }
}

} // namespace typecheck
} // namespace naab
