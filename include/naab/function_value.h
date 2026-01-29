#ifndef NAAB_FUNCTION_VALUE_H
#define NAAB_FUNCTION_VALUE_H

#include <memory>
#include <string>
#include <vector>
#include "naab/ast.h" // For ast::CompoundStmt, ast::Expr, ast::Type

// GEMINI FIX: Forward declaration of interpreter::Value
namespace naab {
namespace interpreter {
    class Value;
    class Environment; // For closure_env
}
}

namespace naab {
namespace interpreter {

// Represents a user-defined function
struct FunctionValue {
    std::string name;
    std::vector<std::string> params; // Parameter names
    std::vector<naab::ast::Type> param_types; // Phase 2.1: Parameter types
    std::vector<naab::ast::Expr*> defaults; // Phase 2.1: Default argument expressions
    std::shared_ptr<naab::ast::CompoundStmt> body;
    std::vector<std::string> type_parameters; // Phase 2.4.1: Generic type parameters
    naab::ast::Type return_type; // Phase 2.4.4: Explicit or inferred return type
    std::string source_file; // Phase 3.1: File where function was defined
    int source_line; // Phase 3.1: Line number where function was defined
    std::shared_ptr<naab::interpreter::Environment> closure_env; // ISS-022: Environment where function was defined (for closures)

    FunctionValue(std::string name,
                  std::vector<std::string> params,
                  std::vector<naab::ast::Type> param_types,
                  std::vector<naab::ast::Expr*> defaults,
                  std::shared_ptr<naab::ast::CompoundStmt> body,
                  std::vector<std::string> type_parameters,
                  naab::ast::Type return_type,
                  std::string source_file,
                  int source_line,
                  std::shared_ptr<naab::interpreter::Environment> closure_env)
        : name(std::move(name)), params(std::move(params)), param_types(std::move(param_types)),
          defaults(std::move(defaults)), body(std::move(body)),
          type_parameters(std::move(type_parameters)),
          return_type(std::move(return_type)),
          source_file(std::move(source_file)), source_line(source_line),
          closure_env(std::move(closure_env)) {}
};

} // namespace interpreter
} // namespace naab

#endif // NAAB_FUNCTION_VALUE_H
