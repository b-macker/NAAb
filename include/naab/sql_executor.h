// NAAb SQL executor -- <<sql>> blocks run in-process on SQLite.
//
// Containment, decided before features:
//   * One in-memory database per process, shared by every <<sql>> block in the
//     run (state carries across blocks, like the embedded Python interpreter).
//     It is never a file, and nothing in SQL can make it one: ATTACH, DETACH
//     and VACUUM INTO are refused by an authorizer, extension loading is off,
//     and temporary storage stays in memory. A <<sql>> block therefore has no
//     filesystem reach at all, so capabilities.filesystem has nothing to gate.
//   * Bound variables (<<sql[x]>>) arrive as SQLite parameters -- :x, @x or $x
//     in the SQL -- never as text spliced into it. Every other language gets
//     its bound variables as generated declaration CODE (vm.cpp
//     buildVarDeclarations, polyglot.cpp); doing that for SQL would be SQL
//     injection by construction, so both engines hand the values over through
//     setPendingSqlBindings() instead.
//   * --timeout reaches a running query through a progress handler, so a
//     runaway recursive CTE stops instead of holding the interpreter.
//   * The sandbox gate is LanguageRegistry::getExecutor()'s, as for every
//     language.
//
// Result: the rows of the LAST statement that returns rows, as a list of
// dicts (column -> value); null when no statement returns rows. INTEGER maps
// to int (to double outside 32-bit range), REAL to double, TEXT and BLOB to
// string, NULL to null.

#pragma once

#include "naab/language_registry.h"
#include "naab/naab_val.h"

#include <string>
#include <utility>
#include <vector>

struct sqlite3;

namespace naab {
namespace runtime {

using SqlBindings = std::vector<std::pair<std::string, interpreter::NaabVal>>;

// Bound variables for the next <<sql>> block executed on THIS thread. The
// executor consumes (and clears) them; a block with no pending bindings runs
// with none.
void setPendingSqlBindings(SqlBindings bindings);

class SqlExecutor : public Executor {
public:
    SqlExecutor();
    ~SqlExecutor() override;

    bool execute(const std::string& code) override;
    interpreter::NaabVal executeWithReturn(const std::string& code) override;
    interpreter::NaabVal callFunction(const std::string& function_name,
                                      const std::vector<interpreter::NaabVal>& args) override;
    bool isInitialized() const override;
    std::string getLanguage() const override { return "sql"; }
    std::string getCapturedOutput() override { return ""; }
    std::string getRuntimeVersion() const override;
    bool runsInProcess() const override { return true; }

    // Rows a single block may return before it is refused (memory bound).
    static constexpr size_t kMaxResultRows = 100000;

private:
    interpreter::NaabVal run(const std::string& code, const SqlBindings& bindings);
    // ONE database for the process: "sql" and "sqlite" are registered as two
    // executor instances, and each opening its own would split the program's
    // tables between two databases depending on which tag a block used.
};

} // namespace runtime
} // namespace naab
