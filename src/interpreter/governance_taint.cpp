// NAAb Interpreter — Governance Taint Tracking Helpers
// Split from interpreter.cpp for maintainability
//
// Contains: expressionContainsTaint, checkRhsTainted, checkRhsSanitized,
//           checkPolyglotBoundVarTaint, checkExpressionTaintedSink

#include "naab/interpreter.h"
#include <fmt/core.h>

namespace naab {
namespace interpreter {

// Governance v4: Recursively check if an expression tree references any tainted variable
// Covers ALL 15 expression AST node types (BUG-S through BUG-Z completeness)
bool Interpreter::expressionContainsTaint(ast::Expr* expr) {
    if (!governance_ || !governance_->isActive()) return false;
    if (!expr) return false;

    // Direct identifier reference
    if (auto* id = dynamic_cast<ast::IdentifierExpr*>(expr)) {
        return governance_->isTainted(id->getName());
    }

    // Binary expression (string concat, subscript, arithmetic, etc.)
    if (auto* bin = dynamic_cast<ast::BinaryExpr*>(expr)) {
        return expressionContainsTaint(bin->getLeft()) ||
               expressionContainsTaint(bin->getRight());
    }

    // BUG-W: Unary expression (!tainted, -tainted)
    if (auto* un = dynamic_cast<ast::UnaryExpr*>(expr)) {
        return expressionContainsTaint(un->getOperand());
    }

    // Function call arguments (e.g., string(tainted_var))
    if (auto* call = dynamic_cast<ast::CallExpr*>(expr)) {
        for (const auto& arg : call->getArgs()) {
            if (expressionContainsTaint(arg.get())) return true;
        }
        if (auto* member = dynamic_cast<ast::MemberExpr*>(call->getCallee())) {
            if (expressionContainsTaint(member->getObject())) return true;
        }
        return false;
    }

    // Member access (e.g., tainted_dict.key)
    if (auto* member = dynamic_cast<ast::MemberExpr*>(expr)) {
        return expressionContainsTaint(member->getObject());
    }

    // BUG-S: String interpolation in LiteralExpr — scan ${...} for tainted variable references
    if (auto* lit = dynamic_cast<ast::LiteralExpr*>(expr)) {
        if (lit->getLiteralKind() == ast::LiteralKind::String) {
            const std::string& raw = lit->getValue();
            size_t pos = 0;
            while ((pos = raw.find("${", pos)) != std::string::npos) {
                pos += 2;
                int depth = 1;
                std::string expr_text;
                size_t i = pos;
                while (i < raw.size() && depth > 0) {
                    if (raw[i] == '{') depth++;
                    else if (raw[i] == '}') { depth--; if (depth == 0) break; }
                    expr_text += raw[i]; i++;
                }
                // Extract identifiers from the interpolated expression text
                std::string word;
                for (char c : expr_text) {
                    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') { word += c; }
                    else {
                        if (!word.empty() && governance_->isTainted(word)) return true;
                        word.clear();
                    }
                }
                if (!word.empty() && governance_->isTainted(word)) return true;
                pos = i + 1;
            }
        }
        return false;
    }

    // BUG-T: If-expression (let x = if cond { tainted } else { clean })
    if (auto* ifex = dynamic_cast<ast::IfExpr*>(expr)) {
        return expressionContainsTaint(ifex->getThenExpr()) ||
               expressionContainsTaint(ifex->getElseExpr());
    }

    // BUG-U: List literal ([tainted, clean])
    if (auto* list = dynamic_cast<ast::ListExpr*>(expr)) {
        for (const auto& elem : list->getElements()) {
            if (expressionContainsTaint(elem.get())) return true;
        }
        return false;
    }

    // BUG-V: Dict literal ({"key": tainted})
    if (auto* dict = dynamic_cast<ast::DictExpr*>(expr)) {
        for (const auto& entry : dict->getEntries()) {
            if (expressionContainsTaint(entry.second.get())) return true;
        }
        return false;
    }

    // BUG-X: Struct literal (new Point { x: tainted, y: 0 })
    if (auto* sl = dynamic_cast<ast::StructLiteralExpr*>(expr)) {
        for (const auto& field : sl->getFieldInits()) {
            if (expressionContainsTaint(field.second.get())) return true;
        }
        return false;
    }

    // BUG-Y: Await expression (await async_func())
    if (auto* aw = dynamic_cast<ast::AwaitExpr*>(expr)) {
        // Check if the inner expression (the call) references tainted vars
        if (expressionContainsTaint(aw->getExpr())) return true;
        // Also check if the async function RETURNED tainted data
        // (runtime check — safe because expressionContainsTaint is always
        //  called after eval() has already executed the expression)
        // FIX-A: Consume-once — reset after reading
        if (governance_ && governance_->lastReturnWasTainted()) {
            governance_->setLastReturnTainted(false);
            return true;
        }
        return false;
    }

    // BUG-Z: Range expression (tainted..10)
    if (auto* rng = dynamic_cast<ast::RangeExpr*>(expr)) {
        return expressionContainsTaint(rng->getStart()) ||
               expressionContainsTaint(rng->getEnd());
    }

    // InlineCodeExpr: handled by isTaintSource("polyglot_output") in VarDeclStmt/Assignment
    // LambdaExpr: body is a block, not a value expression — N/A

    // MatchExpr: check all arm body expressions for tainted returns (FIX for BUG-MatchExpr)
    if (auto* match = dynamic_cast<ast::MatchExpr*>(expr)) {
        // Check if subject itself is tainted
        // Note: Subject taint doesn't propagate to result (only affects control flow)
        // But we check it for completeness
        // if (expressionContainsTaint(match->getSubject())) return true;  // Optional

        // Check if any arm body expression contains taint
        for (auto& arm : match->getArms()) {
            if (expressionContainsTaint(arm.body.get())) return true;
        }
        return false;
    }

    return false;
}

// REFACTOR-1: Unified check — is the RHS expression tainted? (source, propagation, or return taint)
bool Interpreter::checkRhsTainted(ast::Expr* rhs_expr) {
    if (!governance_ || !governance_->isActive() || !rhs_expr) return false;

    // FIX-A: Capture and consume lastReturnWasTainted IMMEDIATELY.
    // This flag was set during eval() by a function call within this statement.
    // We must consume it now regardless of which step returns true, otherwise
    // a step-2 short-circuit (expressionContainsTaint) leaves the flag stale
    // for the next statement (e.g., `let x = 80.0` after tainted dict access).
    bool return_was_tainted = governance_->lastReturnWasTainted();
    governance_->setLastReturnTainted(false);

    // 1. Direct source check (env.get, io.read_line, etc.)
    if (auto* call = dynamic_cast<ast::CallExpr*>(rhs_expr)) {
        if (auto* member = dynamic_cast<ast::MemberExpr*>(call->getCallee())) {
            std::string full_name;
            if (auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member->getObject())) {
                full_name = obj_id->getName() + ".";
            }
            full_name += member->getMember();
            if (governance_->isTaintSource(full_name)) return true;
        }
        if (auto* id = dynamic_cast<ast::IdentifierExpr*>(call->getCallee())) {
            if (governance_->isTaintSource(id->getName())) return true;
        }
    }
    // Polyglot output source (FIX-DX-12: per-language taint)
    if (auto* ice = dynamic_cast<ast::InlineCodeExpr*>(rhs_expr)) {
        if (governance_->isTaintSource("polyglot_output") ||
            governance_->isTaintSource("polyglot_output:" + ice->getLanguage()))
            return true;
    }

    // 2. Propagation (tainted variable in expression tree)
    if (expressionContainsTaint(rhs_expr)) return true;

    // 3. Return taint (any function call in expression tree that returned tainted data)
    if (return_was_tainted) return true;

    return false;
}

// REFACTOR-1: Unified check — is the RHS a sanitizer call?
bool Interpreter::checkRhsSanitized(ast::Expr* rhs_expr) {
    if (!governance_ || !governance_->isActive() || !rhs_expr) return false;
    auto* call = dynamic_cast<ast::CallExpr*>(rhs_expr);
    if (!call) return false;

    if (auto* id = dynamic_cast<ast::IdentifierExpr*>(call->getCallee())) {
        if (governance_->isSanitizer(id->getName())) return true;
    }
    // FIX-DX-3: MemberExpr sanitizers (e.g., utils.sanitize_input())
    if (auto* mem = dynamic_cast<ast::MemberExpr*>(call->getCallee())) {
        std::string full_name;
        if (auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(mem->getObject())) {
            full_name = obj_id->getName() + ".";
        }
        full_name += mem->getMember();
        if (governance_->isSanitizer(full_name)) return true;
    }
    return false;
}

// FIX-D: Deduplicated polyglot bound-variable taint sink check
void Interpreter::checkPolyglotBoundVarTaint(const std::string& language,
    const std::vector<std::string>& bound_vars, int line) {
    if (!governance_ || !governance_->isActive()) return;
    std::string sink_type;
    if (language == "shell" || language == "sh" || language == "bash") {
        sink_type = "shell_exec";
    } else if (language == "js") {
        sink_type = "javascript_exec";
    } else {
        sink_type = language + "_exec";
    }
    for (const auto& bv : bound_vars) {
        std::string terr = governance_->checkTaintedSink(bv, sink_type, current_file_, line);
        if (!terr.empty()) throw std::runtime_error(terr);
    }
}

// BUG-R: Check expression-level taint at sinks (handles both identifiers and complex expressions)
std::string Interpreter::checkExpressionTaintedSink(ast::Expr* expr, const std::string& sink_type,
                                                     const std::string& file, int line) {
    if (!governance_ || !governance_->isActive()) return "";
    if (!expr) return "";

    // Try direct identifier first (for specific variable name in error message)
    if (auto* id = dynamic_cast<ast::IdentifierExpr*>(expr)) {
        return governance_->checkTaintedSink(id->getName(), sink_type, file, line);
    }

    // For complex expressions, check if any sub-expression references tainted data
    if (expressionContainsTaint(expr)) {
        // Can't use checkTaintedSink (needs var name in taint_set_). Check enforcement directly.
        if (!governance_->getRules().taint_tracking.enabled) return "";

        // Check if this sink type is configured
        bool is_sink = false;
        for (const auto& s : governance_->getRules().taint_tracking.sinks) {
            if (sink_type.find(s) != std::string::npos) { is_sink = true; break; }
        }
        if (!is_sink) return "";

        governance_->logTaintDecision("(expression)", "BLOCKED", sink_type, file, line);

        std::string msg = "Taint tracking violation: expression contains untrusted data and reached sink '"
                          + sink_type + "' without sanitization";
        if (!file.empty()) msg += " at " + file + ":" + std::to_string(line);

        const auto& level = governance_->getRules().taint_tracking.level;
        if (level == "hard") return msg;
        if (level == "soft" && !governance_->isOverrideEnabled()) return msg;
        // Advisory: warn only
        fmt::print(stderr, "[GOVERNANCE] WARNING: {}\n", msg);
        return "";
    }

    return "";
}

} // namespace interpreter
} // namespace naab
