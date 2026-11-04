// NAAb Interpreter — Governance Taint Tracking Helpers
// Split from interpreter.cpp for maintainability
//
// Contains: expressionContainsTaint, checkRhsTainted, checkRhsSanitized,
//           checkPolyglotBoundVarTaint, checkExpressionTaintedSink

#include "naab/interpreter.h"
#include "naab/string_interpolation.h"
#include "naab/taint_rule.h"
#include <fmt/core.h>

namespace naab {
namespace interpreter {

// Is this expression's value tainted? The rule is naab/taint_rule.h, shared
// with the VM compiler's compile-time walk (Bundle 12); this adapter supplies
// what the tree-walker knows at run time: the engine's tainted names, its
// sources and sanitizers, and an awaited call's return flag.
bool Interpreter::expressionContainsTaint(ast::Expr* expr) {
    if (!governance_ || !governance_->isActive()) return false;
    if (!expr) return false;

    struct Hooks : taint::RuleHooks {
        governance::GovernanceEngine* gov;
        explicit Hooks(governance::GovernanceEngine* g) : gov(g) {}
        bool nameTainted(const std::string& name) override { return gov->isTainted(name); }
        taint::CallVerdict call(ast::CallExpr& c) override {
            const std::string name = taint::calleeName(c.getCallee());
            if (name.empty()) return taint::CallVerdict::Undecided;
            if (gov->isTaintSource(name)) return taint::CallVerdict::Tainted;
            if (gov->isSanitizer(name)) return taint::CallVerdict::Clean;
            return taint::CallVerdict::Undecided;
        }
        // The async function RETURNED tainted data. Safe as a runtime check:
        // expressionContainsTaint() runs after eval() has executed the
        // expression. Consume-once (FIX-A): reset after reading.
        bool awaitResultTainted(ast::AwaitExpr&) override {
            if (!gov->lastReturnWasTainted()) return false;
            gov->setLastReturnTainted(false);
            return true;
        }
    } hooks(governance_.get());
    return taint::expressionTainted(expr, hooks);
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
            if (governance_->isTaintSource(full_name)) {
                governance_->setLastReturnTainted(true, full_name);
                return true;
            }
        }
        if (auto* id = dynamic_cast<ast::IdentifierExpr*>(call->getCallee())) {
            if (governance_->isTaintSource(id->getName())) {
                governance_->setLastReturnTainted(true, id->getName());
                return true;
            }
        }
    }
    // Polyglot output source
    if (dynamic_cast<ast::InlineCodeExpr*>(rhs_expr) != nullptr) {
        // V-GOV-006: All polyglot block outputs are unconditionally tainted when
        // governance is active. Foreign-language data is untrusted by definition —
        // the marshalling layer (JSON/subprocess) cannot preserve taint metadata,
        // so we conservatively taint all cross-language return values.
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
        // Gap 5/9: Use PREFIX match consistent with governance_checks.cpp
        // Substring match was too broad (e.g., "file" would match "profile.read")
        for (const auto& s : governance_->getRules().taint_tracking.sinks) {
            if (sink_type.size() >= s.size() &&
                sink_type.compare(0, s.size(), s) == 0) { is_sink = true; break; }
        }
        if (!is_sink) return "";

        governance_->logTaintDecision("(expression)", "BLOCKED", sink_type, file, line);

        std::string msg = "Taint tracking violation: expression contains untrusted data and reached sink '"
                          + sink_type + "' without sanitization";
        if (!file.empty()) msg += " at " + file + ":" + std::to_string(line);

        // Add sanitizer guidance (don't list specific sanitizers — adversarial LLMs
        // would use the list to create identity-function bypasses)
        msg += "\n\n  Help: Pass the tainted expression through a real sanitization function\n"
               "  before using it in a '" + sink_type + "' sink. Sanitizers must actually\n"
               "  validate or transform the data — identity functions are detected.\n";

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
