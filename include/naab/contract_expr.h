#pragma once
// Contract clauses: contracts.<fn>.requires / ensures in govern.json (Bundle 11).
//
// A clause is one NAAb expression. `requires` holds over the arguments when
// the function is entered (after its defaults are filled), `ensures` over the
// arguments as they were on entry and the returned value, named `result`.
// The VM compiles each clause into a function of those names and runs it at
// the call boundary, so a clause means exactly what the same expression means
// in a program -- one arithmetic, one `==` -- instead of a second evaluator.
//
// A clause must be PURE: it is checked on every call, and a check that can
// write a file, call a model or change an argument is not a check. The rule is
// a whitelist over the syntax tree, applied when govern.json loads (a clause
// outside it refuses the config, exit 4):
//   literals (a string may not interpolate), names, lists, dicts, unary and
//   binary operators except assignment and |>, indexing, `in`, `??`, member
//   access, and calls of len(), type() and typeof() only.
// The names a clause may use are the function's parameters (and `result` in
// ensures); that half is checked when the function is compiled, the first
// moment both are known. The VM also refuses, while a clause runs, any call
// that is not one of those three builtins and any member access on a value
// that is not a dict -- a parameter can hold a module or a block, and a user
// function can shadow `len`.
#include <memory>
#include <string>
#include <vector>

namespace naab {
namespace ast { class Expr; }
namespace contracts {

// The builtins a clause may call.
bool isPureContractBuiltin(const std::string& name);

// Parse `text` as one contract expression. On success returns the tree and
// fills `names` with every identifier it reads (in order, duplicates removed);
// on failure returns null and sets `why` to the reason (a parse error, or the
// construct the whitelist refuses).
std::unique_ptr<ast::Expr> parseContractClause(const std::string& text,
                                               std::vector<std::string>& names,
                                               std::string& why);

// The integer literals a clause compares against (for the counterexample
// search's boundary values); empty when it has none or does not parse.
std::vector<long long> contractClauseIntLiterals(const std::string& text);

}  // namespace contracts
}  // namespace naab
