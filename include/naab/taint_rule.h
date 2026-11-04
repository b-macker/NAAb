// NAAb -- the static taint rule: is an expression's value tainted? (Bundle 12)
//
// One rule, used by both AST walks: the tree-walker's
// Interpreter::expressionContainsTaint() and the VM compiler's
// Compiler::exprContainsTaint() (which marks values at compile time; the VM's
// runtime taint stack is the third implementation and is pinned to this one by
// tests/governance_v4/test_taint_semantics.sh, generated from
// tests/governance_v4/taint_semantics.psv). Each walker passes its own lookups
// -- which names are tainted, what a call's callee decides -- and the rule says
// how taint flows through each kind of expression.
//
// Before this, the two walks were separate if-chains ending in `return false`,
// so a kind either one did not name was clean by construction: try-as-
// expression laundered taint on the tree-walker (campaign finding 15), so did an
// inline polyglot block in a sink argument (A21), and the compiler's walk knew
// no match, try, yield, range or struct literal at all. The switch below names
// every NodeKind and has no default, so -Wswitch (on with -Wall) reports a kind
// added later, and anything that reaches the end -- a new kind before it is
// added here, or a statement handed in as an expression -- is TAINTED.
//
// The semantics, per kind (owner's decisions 2026-11-04):
//   - a value is tainted when any operand that flows into it is tainted;
//   - conditions count: an if-condition, a match subject, its patterns and its
//     guards taint the result (an implicit flow), on both engines;
//   - a call is tainted when its callee is a configured source, clean when it is
//     a configured sanitizer, otherwise tainted when any argument or the callee
//     expression (a method's receiver) is;
//   - a polyglot block's value is tainted whatever its inputs (V-GOV-006: the
//     marshalling cannot carry taint, so foreign output is untrusted), as both
//     runtimes already treat it;
//   - a lambda's value is clean: a function holds no data until it is called,
//     and the call is judged by what it returns;
//   - throw carries its operand's taint (the catch binding receives the value).
#pragma once

#include "naab/ast.h"
#include "naab/string_interpolation.h"

#include <string>

namespace naab {
namespace taint {

enum class CallVerdict { Undecided, Tainted, Clean };

// What a walker knows that the rule does not.
struct RuleHooks {
    virtual ~RuleHooks() = default;
    // Is the variable of this name tainted here?
    virtual bool nameTainted(const std::string& name) = 0;
    // A call's callee: a configured source (Tainted), a sanitizer (Clean), or
    // neither (Undecided: the arguments and the callee expression decide).
    virtual CallVerdict call(ast::CallExpr& call) = 0;
    // Runtime knowledge an awaited call's return value carries (the
    // tree-walker's return flag); a compile-time walk knows none.
    virtual bool awaitResultTainted(ast::AwaitExpr&) { return false; }
};

// The dotted name a callee is configured under: `env.get`, `sanitize_x`.
inline std::string calleeName(ast::Expr* callee) {
    if (auto* id = dynamic_cast<ast::IdentifierExpr*>(callee)) return id->getName();
    if (auto* mem = dynamic_cast<ast::MemberExpr*>(callee)) {
        std::string full;
        if (auto* obj = dynamic_cast<ast::IdentifierExpr*>(mem->getObject()))
            full = obj->getName() + ".";
        return full + mem->getMember();
    }
    return "";
}

inline bool expressionTainted(ast::Expr* expr, RuleHooks& h) {
    if (!expr) return false;
    using K = ast::NodeKind;
    switch (expr->getKind()) {
        case K::IdentifierExpr:
            return h.nameTainted(static_cast<ast::IdentifierExpr*>(expr)->getName());

        case K::LiteralExpr: {
            auto* lit = static_cast<ast::LiteralExpr*>(expr);
            if (lit->getLiteralKind() != ast::LiteralKind::String) return false;
            // The evaluator's own splitter, so an escaped \${x} is not a use of x.
            for (const auto& id : interp::interpolatedIdentifiers(lit->getValue()))
                if (h.nameTainted(id)) return true;
            return false;
        }

        case K::BinaryExpr: {  // arithmetic, comparison, &&, ||, ??, subscript, concat
            auto* bin = static_cast<ast::BinaryExpr*>(expr);
            return expressionTainted(bin->getLeft(), h) || expressionTainted(bin->getRight(), h);
        }

        case K::UnaryExpr:
            return expressionTainted(static_cast<ast::UnaryExpr*>(expr)->getOperand(), h);

        case K::CallExpr: {
            auto* call = static_cast<ast::CallExpr*>(expr);
            switch (h.call(*call)) {
                case CallVerdict::Tainted: return true;
                case CallVerdict::Clean: return false;
                case CallVerdict::Undecided: break;
            }
            for (const auto& arg : call->getArgs())
                if (expressionTainted(arg.get(), h)) return true;
            // A method's receiver (`t.upper()`); a module name or a function
            // name is not tainted, so a plain call is decided by its arguments.
            return expressionTainted(call->getCallee(), h);
        }

        case K::MemberExpr:
            return expressionTainted(static_cast<ast::MemberExpr*>(expr)->getObject(), h);

        case K::DictExpr:
            for (const auto& kv : static_cast<ast::DictExpr*>(expr)->getEntries())
                if (expressionTainted(kv.first.get(), h) || expressionTainted(kv.second.get(), h))
                    return true;
            return false;

        case K::ListExpr:
            for (const auto& e : static_cast<ast::ListExpr*>(expr)->getElements())
                if (expressionTainted(e.get(), h)) return true;
            return false;

        case K::RangeExpr: {
            auto* r = static_cast<ast::RangeExpr*>(expr);
            return expressionTainted(r->getStart(), h) || expressionTainted(r->getEnd(), h);
        }

        case K::StructLiteralExpr:
            for (const auto& f : static_cast<ast::StructLiteralExpr*>(expr)->getFieldInits())
                if (expressionTainted(f.second.get(), h)) return true;
            return false;

        case K::InlineCodeExpr:
            return true;  // V-GOV-006: foreign output is tainted, inputs or not

        case K::IfExpr: {  // conditions count
            auto* ife = static_cast<ast::IfExpr*>(expr);
            return expressionTainted(ife->getCondition(), h) ||
                   expressionTainted(ife->getThenExpr(), h) ||
                   expressionTainted(ife->getElseExpr(), h);
        }

        case K::LambdaExpr:
            return false;  // a function value holds no data; its calls are judged

        case K::MatchExpr: {  // conditions count: subject, patterns, guards
            auto* m = static_cast<ast::MatchExpr*>(expr);
            if (expressionTainted(m->getSubject(), h)) return true;
            for (auto& arm : m->getArms())
                if (expressionTainted(arm.pattern.get(), h) ||
                    expressionTainted(arm.guard.get(), h) ||
                    expressionTainted(arm.body.get(), h))
                    return true;
            return false;
        }

        case K::AwaitExpr: {
            auto* aw = static_cast<ast::AwaitExpr*>(expr);
            return expressionTainted(aw->getExpr(), h) || h.awaitResultTainted(*aw);
        }

        case K::YieldExpr:
            return expressionTainted(static_cast<ast::YieldExpr*>(expr)->getExpr(), h);

        case K::TryCatchExpr: {
            auto* tc = static_cast<ast::TryCatchExpr*>(expr);
            return expressionTainted(tc->getTryExpr(), h) || expressionTainted(tc->getCatchExpr(), h);
        }

        case K::ThrowExpr:
            return expressionTainted(static_cast<ast::ThrowExpr*>(expr)->getExpr(), h);

        // Statements are not values: one handed in here is a caller's mistake,
        // judged as an unknown kind.
        case K::Program: case K::UseStatement: case K::FunctionDecl: case K::MainBlock:
        case K::StructDecl: case K::EnumDecl: case K::InterfaceDecl: case K::CompoundStmt:
        case K::ExprStmt: case K::ReturnStmt: case K::IfStmt: case K::ForStmt:
        case K::WhileStmt: case K::BreakStmt: case K::ContinueStmt: case K::VarDeclStmt:
        case K::ImportStmt: case K::ExportStmt: case K::TryStmt: case K::ThrowStmt:
        case K::ModuleUseStmt: case K::FunctionDeclStmt: case K::StructDeclStmt:
        case K::RuntimeDeclStmt: case K::DestructureStmt:
            break;
    }
    return true;  // fail closed: a kind this rule does not name is tainted
}

}  // namespace taint
}  // namespace naab
