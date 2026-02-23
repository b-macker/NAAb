// AST Node Implementation
// Visitor pattern implementation for all AST nodes

#include "naab/ast.h"

namespace naab {
namespace ast {

// Implement accept() methods for visitor pattern

void UseStatement::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void FunctionDecl::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void MainBlock::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void CompoundStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void ExprStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void ReturnStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void IfStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void ForStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void WhileStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void BreakStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void ContinueStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void VarDeclStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void ImportStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void ExportStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void TryStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void ThrowStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void ModuleUseStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void BinaryExpr::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

Type BinaryExpr::getType() const {
    // Cached type from TypeChecker available via getCachedType() (Phase 4)
    // For now, return Any as fallback (type info primarily used by TypeChecker)
    return Type::makeAny();
}

void UnaryExpr::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

Type UnaryExpr::getType() const {
    // Use cached type from TypeChecker if available (Phase 4)
    // For now, return Any as fallback
    return Type::makeAny();
}

void CallExpr::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

Type CallExpr::getType() const {
    // Use cached type from TypeChecker if available (Phase 4)
    // For now, return Any as fallback
    return Type::makeAny();
}

void MemberExpr::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

Type MemberExpr::getType() const {
    // Use cached type from TypeChecker if available (Phase 4)
    // For now, return Any as fallback
    return Type::makeAny();
}

void IdentifierExpr::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

Type IdentifierExpr::getType() const {
    // Use cached type from TypeChecker if available (Phase 4)
    // Symbol table lookup requires TypeChecker context
    return Type::makeAny();
}

void LiteralExpr::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

Type LiteralExpr::getType() const {
    switch (kind_) {
        case LiteralKind::Int: return Type::makeInt();
        case LiteralKind::Float: return Type::makeFloat();
        case LiteralKind::String: return Type::makeString();
        case LiteralKind::Bool: return Type::makeBool();
        default: return Type::makeAny();
    }
}

void DictExpr::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

Type DictExpr::getType() const {
    // Use cached type from TypeChecker if available (Phase 4)
    // For now, return Any as fallback
    return Type::makeAny();
}

void ListExpr::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

Type ListExpr::getType() const {
    // Use cached type from TypeChecker if available (Phase 4)
    // For now, return Any as fallback
    return Type::makeAny();
}

void RangeExpr::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

Type RangeExpr::getType() const {
    // Range type - for now return Any, will be implemented as iterator
    return Type::makeAny();
}

void StructDecl::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void EnumDecl::accept(ASTVisitor& visitor) {  // Phase 2.4.3
    visitor.visit(*this);
}

void StructLiteralExpr::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void InlineCodeExpr::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void IfExpr::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void LambdaExpr::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void MatchExpr::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void Program::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void FunctionDeclStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void StructDeclStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

void RuntimeDeclStmt::accept(ASTVisitor& visitor) {
    visitor.visit(*this);
}

} // namespace ast
} // namespace naab
