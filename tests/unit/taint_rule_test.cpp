// The static taint rule (include/naab/taint_rule.h, Bundle 12): an expression
// kind the rule does not name is TAINTED. No NAAb program can produce such a node
// -- the parser builds only the kinds in ast::NodeKind, and every one is named --
// so this is the one property the generated suite
// (tests/governance_v4/test_taint_semantics.sh) cannot reach. Each fail-closed
// case is beside a control that the rule is not simply "everything is tainted".
#include "naab/taint_rule.h"

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>

namespace {

using naab::ast::NodeKind;

// An expression of any kind, including ones the enum does not have.
class FakeExpr : public naab::ast::Expr {
public:
    explicit FakeExpr(NodeKind k) : Expr(k) {}
    void accept(naab::ast::ASTVisitor&) override {}
    naab::ast::Type getType() const override { return naab::ast::Type::makeAny(); }
};

struct Names : naab::taint::RuleHooks {
    std::set<std::string> tainted;
    bool nameTainted(const std::string& n) override { return tainted.count(n) > 0; }
    naab::taint::CallVerdict call(naab::ast::CallExpr&) override {
        return naab::taint::CallVerdict::Undecided;
    }
};

TEST(TaintRule, UnknownKindFailsClosed) {
    Names h;
    FakeExpr unknown(static_cast<NodeKind>(9999));
    EXPECT_TRUE(naab::taint::expressionTainted(&unknown, h));
}

TEST(TaintRule, StatementKindHandedInAsAnExpressionFailsClosed) {
    Names h;
    FakeExpr stmt(NodeKind::BreakStmt);
    EXPECT_TRUE(naab::taint::expressionTainted(&stmt, h));
}

TEST(TaintRule, KnownKindsAreJudgedNotDefaulted) {
    Names h;
    h.tainted.insert("t");
    naab::ast::IdentifierExpr tainted("t"), clean("c");
    EXPECT_TRUE(naab::taint::expressionTainted(&tainted, h));
    EXPECT_FALSE(naab::taint::expressionTainted(&clean, h));  // control
    EXPECT_FALSE(naab::taint::expressionTainted(nullptr, h));  // no expression, no value
}

}  // namespace
