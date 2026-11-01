// contracts.<fn>.requires / ensures: the clause parser and its purity
// whitelist (include/naab/contract_expr.h, Bundle 11). The loader refuses a
// clause this rejects (exit 4); a clause this accepts becomes a function the
// VM runs at every call, so a false accept is a check with an effect.
#include "naab/contract_expr.h"
#include "naab/ast.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using naab::contracts::contractClauseIntLiterals;
using naab::contracts::isPureContractBuiltin;
using naab::contracts::parseContractClause;

namespace {

bool accepts(const std::string& text, std::vector<std::string>* names_out = nullptr) {
    std::vector<std::string> names;
    std::string why;
    auto e = parseContractClause(text, names, why);
    if (names_out) *names_out = names;
    return e != nullptr && why.empty();
}

std::string refusal(const std::string& text) {
    std::vector<std::string> names;
    std::string why;
    auto e = parseContractClause(text, names, why);
    return e ? std::string() : why;
}

}  // namespace

TEST(ContractExpr, AcceptsPureExpressions) {
    for (const char* t : {"n >= 0", "a + b == result", "len(xs) > 0 && xs[0] != null",
                          "type(x) == \"int\"", "typeof(x) != \"null\"", "!(n < 0)",
                          "k in d", "d.k ?? 0", "[a, b] == [1, 2]", "{\"k\": n} != null",
                          "-n <= 5", "n % 2 == 0 || n == 1"}) {
        EXPECT_TRUE(accepts(t)) << t << ": " << refusal(t);
    }
}

TEST(ContractExpr, CollectsTheNamesItReads) {
    std::vector<std::string> names;
    ASSERT_TRUE(accepts("len(xs) > i && xs[i] == result", &names));
    EXPECT_EQ(names, (std::vector<std::string>{"len", "xs", "i", "result"}));
}

TEST(ContractExpr, RefusesEveryEffect) {
    // Each of these, accepted, would run code or change a value on every call.
    struct Case { const char* text; const char* says; };
    for (const Case& c : std::vector<Case>{
             {"print(n) == null", "print"},
             {"f(n)", "f()"},
             {"xs.push(1) == null", "method call"},
             {"n = 1", "assignment"},
             {"n |> len", "pipeline"},
             {"\"${n}\" == \"1\"", "interpolates"},
             {"fn(x) { return x } != null", ""},
             {"1..3 == null", "range"},
         }) {
        std::string why = refusal(c.text);
        EXPECT_FALSE(why.empty()) << c.text << " was accepted";
        if (*c.says) EXPECT_NE(why.find(c.says), std::string::npos) << c.text << ": " << why;
    }
}

TEST(ContractExpr, RefusesTextThatIsNotOneExpression) {
    EXPECT_FALSE(refusal("n >= 0 )").empty());
    EXPECT_FALSE(refusal("n >= 0; m").empty());
    EXPECT_FALSE(refusal("").empty());
    EXPECT_TRUE(refusal("n >= 0  // a comment").empty());
}

TEST(ContractExpr, OnlyThreeBuiltinsArePure) {
    EXPECT_TRUE(isPureContractBuiltin("len"));
    EXPECT_TRUE(isPureContractBuiltin("type"));
    EXPECT_TRUE(isPureContractBuiltin("typeof"));
    for (const char* n : {"print", "println", "range", "int", "gc_collect", "__slice", ""}) {
        EXPECT_FALSE(isPureContractBuiltin(n)) << n;
    }
}

TEST(ContractExpr, IntLiteralsForBoundaryInputs) {
    EXPECT_EQ(contractClauseIntLiterals("n >= 10 && n < -3"), (std::vector<long long>{-3, 10}));
    EXPECT_TRUE(contractClauseIntLiterals("print(n)").empty());  // does not parse as a clause
}
