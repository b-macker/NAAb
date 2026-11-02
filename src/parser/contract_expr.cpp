// Contract clause parsing and the purity whitelist (include/naab/contract_expr.h).
#include "naab/contract_expr.h"

#include "naab/ast.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/string_interpolation.h"

#include <algorithm>
#include <exception>

namespace naab {
namespace contracts {

bool isPureContractBuiltin(const std::string& name) {
    return name == "len" || name == "type" || name == "typeof";
}

namespace {

void addName(std::vector<std::string>& names, const std::string& n) {
    if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
}

// Returns "" when `e` is inside the whitelist, else the construct refused.
// A node type this does not name is refused: a new kind of expression is
// impure until someone decides otherwise.
std::string checkPure(ast::Expr* e, std::vector<std::string>& names, int depth) {
    if (!e) return "an empty expression";
    if (depth > 200) return "an expression nested too deeply";
    if (auto* lit = dynamic_cast<ast::LiteralExpr*>(e)) {
        if (lit->getLiteralKind() == ast::LiteralKind::String &&
            interp::hasInterpolation(lit->getValue())) {
            return "a string that interpolates ${...} (it runs code)";
        }
        return "";
    }
    if (auto* id = dynamic_cast<ast::IdentifierExpr*>(e)) {
        addName(names, id->getName());
        return "";
    }
    if (auto* un = dynamic_cast<ast::UnaryExpr*>(e)) {
        return checkPure(un->getOperand(), names, depth + 1);
    }
    if (auto* bin = dynamic_cast<ast::BinaryExpr*>(e)) {
        switch (bin->getOp()) {
            case ast::BinaryOp::Assign:   return "an assignment";
            case ast::BinaryOp::Pipeline: return "a pipeline (|> calls a function)";
            default: break;
        }
        std::string w = checkPure(bin->getLeft(), names, depth + 1);
        if (!w.empty()) return w;
        return checkPure(bin->getRight(), names, depth + 1);
    }
    if (auto* mem = dynamic_cast<ast::MemberExpr*>(e)) {
        return checkPure(mem->getObject(), names, depth + 1);
    }
    if (auto* list = dynamic_cast<ast::ListExpr*>(e)) {
        for (auto& el : list->getElements()) {
            std::string w = checkPure(el.get(), names, depth + 1);
            if (!w.empty()) return w;
        }
        return "";
    }
    if (auto* dict = dynamic_cast<ast::DictExpr*>(e)) {
        for (auto& kv : dict->getEntries()) {
            std::string w = checkPure(kv.first.get(), names, depth + 1);
            if (!w.empty()) return w;
            w = checkPure(kv.second.get(), names, depth + 1);
            if (!w.empty()) return w;
        }
        return "";
    }
    if (auto* call = dynamic_cast<ast::CallExpr*>(e)) {
        auto* callee = dynamic_cast<ast::IdentifierExpr*>(call->getCallee());
        if (!callee) return "a method call (only len(), type() and typeof() may be called)";
        if (!isPureContractBuiltin(callee->getName())) {
            return "a call of " + callee->getName() + "() (only len(), type() and typeof() may be called)";
        }
        for (auto& a : call->getArgs()) {
            std::string w = checkPure(a.get(), names, depth + 1);
            if (!w.empty()) return w;
        }
        return "";
    }
    if (dynamic_cast<ast::InlineCodeExpr*>(e)) return "a polyglot block";
    if (dynamic_cast<ast::LambdaExpr*>(e)) return "a function";
    if (dynamic_cast<ast::AwaitExpr*>(e)) return "await";
    if (dynamic_cast<ast::YieldExpr*>(e)) return "yield";
    if (dynamic_cast<ast::ThrowExpr*>(e)) return "throw";
    if (dynamic_cast<ast::TryCatchExpr*>(e)) return "try";
    if (dynamic_cast<ast::StructLiteralExpr*>(e)) return "a struct literal (it calls the struct's constructor)";
    if (dynamic_cast<ast::RangeExpr*>(e)) return "a range";
    if (dynamic_cast<ast::IfExpr*>(e)) return "an if expression";
    if (dynamic_cast<ast::MatchExpr*>(e)) return "a match expression";
    return "an expression of a kind contract clauses do not allow";
}

}  // namespace

std::unique_ptr<ast::Expr> parseContractClause(const std::string& text,
                                               std::vector<std::string>& names,
                                               std::string& why) {
    names.clear();
    why.clear();
    std::unique_ptr<ast::Expr> expr;
    try {
        lexer::Lexer lex(text);
        auto tokens = lex.tokenize();
        parser::Parser p(tokens);
        p.setSource(text, "<contract>");
        expr = p.parseExpressionOnly();
    } catch (const std::exception& e) {
        why = std::string("it does not parse as one NAAb expression: ") + e.what();
        // Parser messages carry a source excerpt across several lines; keep
        // the first so the config error stays readable.
        auto nl = why.find('\n');
        if (nl != std::string::npos) why.resize(nl);
        return nullptr;
    }
    if (!expr) {
        why = "it does not parse as one NAAb expression";
        return nullptr;
    }
    std::string refused = checkPure(expr.get(), names, 0);
    if (!refused.empty()) {
        why = "it contains " + refused + "; a contract clause must not have effects";
        return nullptr;
    }
    return expr;
}

std::vector<long long> contractClauseIntLiterals(const std::string& text) {
    std::vector<long long> out;
    std::vector<std::string> names;
    std::string why;
    auto expr = parseContractClause(text, names, why);
    if (!expr) return out;
    std::vector<ast::Expr*> stack{expr.get()};
    while (!stack.empty()) {
        ast::Expr* e = stack.back();
        stack.pop_back();
        if (!e) continue;
        if (auto* un = dynamic_cast<ast::UnaryExpr*>(e)) {
            auto* lit = dynamic_cast<ast::LiteralExpr*>(un->getOperand());
            if (un->getOp() == ast::UnaryOp::Neg && lit &&
                lit->getLiteralKind() == ast::LiteralKind::Int) {
                try { out.push_back(-std::stoll(lit->getValue())); } catch (...) {}
                continue;
            }
            stack.push_back(un->getOperand());
        } else if (auto* lit = dynamic_cast<ast::LiteralExpr*>(e)) {
            if (lit->getLiteralKind() == ast::LiteralKind::Int) {
                try { out.push_back(std::stoll(lit->getValue())); } catch (...) {}
            }
        } else if (auto* bin = dynamic_cast<ast::BinaryExpr*>(e)) {
            stack.push_back(bin->getLeft());
            stack.push_back(bin->getRight());
        } else if (auto* mem = dynamic_cast<ast::MemberExpr*>(e)) {
            stack.push_back(mem->getObject());
        } else if (auto* call = dynamic_cast<ast::CallExpr*>(e)) {
            for (auto& a : call->getArgs()) stack.push_back(a.get());
        } else if (auto* list = dynamic_cast<ast::ListExpr*>(e)) {
            for (auto& el : list->getElements()) stack.push_back(el.get());
        } else if (auto* dict = dynamic_cast<ast::DictExpr*>(e)) {
            for (auto& kv : dict->getEntries()) {
                stack.push_back(kv.first.get());
                stack.push_back(kv.second.get());
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

}  // namespace contracts
}  // namespace naab
