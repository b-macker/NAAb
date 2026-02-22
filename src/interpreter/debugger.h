#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>
#include <memory>
#include <functional>

// Forward declarations
namespace naab {
namespace ast {
    class ASTNode;
}
namespace interpreter {
    class Environment;
    class Value;
}
}

namespace naab {
namespace interpreter {

// Debugger commands
enum class DebugCommand {
    Continue,    // c, continue - Continue execution
    Step,        // s, step - Step to next line
    Next,        // n, next - Step over function calls
    Vars,        // v, vars - Show all local variables
    Print,       // p <expr> - Print expression
    Watch,       // w <expr> - Add watch expression
    Breakpoint,  // b <file>:<line> - Set breakpoint
    Help,        // h, help - Show all commands
    Quit,        // q, quit - Exit debugger
    Unknown
};

// Interactive debugger for NAAb interpreter
class InterpreterDebugger {
public:
    InterpreterDebugger();

    // Breakpoint management
    void setBreakpoint(const std::string& file, size_t line);
    void clearBreakpoint(const std::string& file, size_t line);
    void listBreakpoints() const;

    // Watch expressions
    void addWatchExpression(const std::string& expr);
    void removeWatchExpression(const std::string& expr);
    void listWatchExpressions() const;

    // Called by interpreter at each statement
    void onStatement(const ast::ASTNode& node, Environment& env);

    // Enable/disable debugging
    void enable() { enabled_ = true; }
    void disable() { enabled_ = false; }
    bool isEnabled() const { return enabled_; }

    // Set expression evaluator callback
    using ExprEvaluator = std::function<std::shared_ptr<Value>(const std::string&, Environment&)>;
    void setExprEvaluator(ExprEvaluator evaluator) {
        expr_evaluator_ = std::move(evaluator);
    }

private:
    bool enabled_;
    bool stepping_;
    // size_t step_count_;  // Reserved for future step counting feature

    // Breakpoints: file path -> set of line numbers
    std::map<std::string, std::set<size_t>> breakpoints_;

    // Watch expressions
    std::vector<std::string> watch_expressions_;

    // Expression evaluator callback
    ExprEvaluator expr_evaluator_;

    // REPL interaction
    void enterREPL(Environment& env, const ast::ASTNode& current_node);
    void printWatchExpressions(Environment& env);
    void printLocalVariables(Environment& env);
    void printHelp() const;

    // Command parsing
    DebugCommand parseCommand(const std::string& input, std::string& arg) const;

    // Check if we should break at this node
    bool shouldBreak(const ast::ASTNode& node) const;

    // Format node location
    std::string formatLocation(const ast::ASTNode& node) const;
};

} // namespace interpreter
} // namespace naab

