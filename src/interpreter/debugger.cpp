#include "naab/ast.h"
// #include "naab/environment.h"  // Removed: Environment is in interpreter.h via value.h
#include "naab/value.h"
#include "debugger.h"
#include <fmt/core.h>
#include <iostream>
#include <sstream>
#include <algorithm>

namespace naab {
namespace interpreter {

// ============================================================================
// InterpreterDebugger Implementation
// ============================================================================

InterpreterDebugger::InterpreterDebugger()
    : enabled_(false), stepping_(false) {}

void InterpreterDebugger::setBreakpoint(const std::string& file, size_t line) {
    breakpoints_[file].insert(line);
    fmt::print("Breakpoint set at {}:{}\n", file, line);
}

void InterpreterDebugger::clearBreakpoint(const std::string& file, size_t line) {
    auto it = breakpoints_.find(file);
    if (it != breakpoints_.end()) {
        it->second.erase(line);
        if (it->second.empty()) {
            breakpoints_.erase(it);
        }
        fmt::print("Breakpoint cleared at {}:{}\n", file, line);
    }
}

void InterpreterDebugger::listBreakpoints() const {
    if (breakpoints_.empty()) {
        fmt::print("No breakpoints set.\n");
        return;
    }

    fmt::print("Breakpoints:\n");
    for (const auto& [file, lines] : breakpoints_) {
        for (size_t line : lines) {
            fmt::print("  {}:{}\n", file, line);
        }
    }
}

void InterpreterDebugger::addWatchExpression(const std::string& expr) {
    if (std::find(watch_expressions_.begin(), watch_expressions_.end(), expr) == watch_expressions_.end()) {
        watch_expressions_.push_back(expr);
        fmt::print("Added watch: {}\n", expr);
    }
}

void InterpreterDebugger::removeWatchExpression(const std::string& expr) {
    watch_expressions_.erase(
        std::remove(watch_expressions_.begin(), watch_expressions_.end(), expr),
        watch_expressions_.end()
    );
    fmt::print("Removed watch: {}\n", expr);
}

void InterpreterDebugger::listWatchExpressions() const {
    if (watch_expressions_.empty()) {
        fmt::print("No watch expressions.\n");
        return;
    }

    fmt::print("Watch expressions:\n");
    for (const auto& expr : watch_expressions_) {
        fmt::print("  {}\n", expr);
    }
}

void InterpreterDebugger::onStatement(const ast::ASTNode& node, Environment& env) {
    if (!enabled_) {
        return;
    }

    // Check if we should break
    bool should_break = shouldBreak(node) || stepping_;

    if (should_break) {
        fmt::print("\nBreakpoint hit at {}\n", formatLocation(node));

        // Print watch expressions if any
        if (!watch_expressions_.empty()) {
            printWatchExpressions(env);
        }

        // Enter REPL
        enterREPL(env, node);

        // Reset stepping flag (in case we were stepping)
        stepping_ = false;
    }
}

bool InterpreterDebugger::shouldBreak(const ast::ASTNode& node) const {
    auto loc = node.getLocation();

    if (loc.filename.empty()) {
        return false;
    }

    auto it = breakpoints_.find(loc.filename);
    if (it != breakpoints_.end()) {
        return it->second.count(loc.line) > 0;
    }

    return false;
}

std::string InterpreterDebugger::formatLocation(const ast::ASTNode& node) const {
    auto loc = node.getLocation();
    return fmt::format("{}:{}:{}", loc.filename, loc.line, loc.column);
}

void InterpreterDebugger::enterREPL(Environment& env, const ast::ASTNode& current_node) {
    (void)current_node;  // Unused - full implementation would show current node context
    fmt::print("\n--- Debug REPL ---\n");
    fmt::print("Type 'h' for help\n\n");

    bool continue_execution = false;

    while (!continue_execution) {
        fmt::print("(debug) ");
        std::string input;
        std::getline(std::cin, input);

        if (input.empty()) {
            continue;
        }

        std::string arg;
        DebugCommand cmd = parseCommand(input, arg);

        switch (cmd) {
            case DebugCommand::Continue:
                fmt::print("Continuing execution...\n");
                continue_execution = true;
                stepping_ = false;
                break;

            case DebugCommand::Step:
                fmt::print("Stepping to next statement...\n");
                continue_execution = true;
                stepping_ = true;
                break;

            case DebugCommand::Next:
                fmt::print("Stepping over (not yet implemented, using step)...\n");
                continue_execution = true;
                stepping_ = true;
                break;

            case DebugCommand::Vars:
                printLocalVariables(env);
                break;

            case DebugCommand::Print: {
                if (arg.empty()) {
                    fmt::print("Error: No expression specified\n");
                    break;
                }

                if (expr_evaluator_) {
                    try {
                        auto result = expr_evaluator_(arg, env);
                        if (result) {
                            fmt::print("{}\n", result->toString());
                        } else {
                            fmt::print("null\n");
                        }
                    } catch (const std::exception& e) {
                        fmt::print("Error evaluating expression: {}\n", e.what());
                    }
                } else {
                    fmt::print("Expression evaluation not available\n");
                }
                break;
            }

            case DebugCommand::Watch:
                if (arg.empty()) {
                    listWatchExpressions();
                } else {
                    addWatchExpression(arg);
                }
                break;

            case DebugCommand::Breakpoint:
                if (arg.empty()) {
                    listBreakpoints();
                } else {
                    // Parse "file:line" format
                    size_t colon_pos = arg.find(':');
                    if (colon_pos != std::string::npos) {
                        std::string file = arg.substr(0, colon_pos);
                        size_t line = std::stoul(arg.substr(colon_pos + 1));
                        setBreakpoint(file, line);
                    } else {
                        fmt::print("Error: Invalid format. Use file:line\n");
                    }
                }
                break;

            case DebugCommand::Help:
                printHelp();
                break;

            case DebugCommand::Quit:
                fmt::print("Exiting debugger...\n");
                std::exit(0);
                break;

            case DebugCommand::Unknown:
                fmt::print("Unknown command: {}\n", input);
                fmt::print("Type 'h' for help\n");
                break;
        }
    }
}

void InterpreterDebugger::printWatchExpressions(Environment& env) {
    if (watch_expressions_.empty()) {
        return;
    }

    fmt::print("\n--- Watch Expressions ---\n");

    for (const auto& expr : watch_expressions_) {
        if (expr_evaluator_) {
            try {
                auto result = expr_evaluator_(expr, env);
                if (result) {
                    fmt::print("  {} = {}\n", expr, result->toString());
                } else {
                    fmt::print("  {} = null\n", expr);
                }
            } catch (const std::exception& e) {
                fmt::print("  {} = <error: {}>\n", expr, e.what());
            }
        } else {
            fmt::print("  {} = <not available>\n", expr);
        }
    }

    fmt::print("\n");
}

void InterpreterDebugger::printLocalVariables(Environment& env) {
    (void)env;  // Unused - full implementation would query environment for variables
    fmt::print("\n--- Local Variables ---\n");

    // Get all variables from current scope
    // This would need to be implemented in the Environment class
    // For now, show a placeholder message

    fmt::print("Variable inspection not yet fully implemented\n");
    fmt::print("Use 'p <var>' to print specific variables\n");

    fmt::print("\n");
}

void InterpreterDebugger::printHelp() const {
    fmt::print("\n--- Debugger Commands ---\n");
    fmt::print("  c, continue      Continue execution\n");
    fmt::print("  s, step          Step to next statement\n");
    fmt::print("  n, next          Step over function calls\n");
    fmt::print("  v, vars          Show all local variables\n");
    fmt::print("  p <expr>         Print expression value\n");
    fmt::print("  w [expr]         Add watch expression (or list watches)\n");
    fmt::print("  b [file:line]    Set breakpoint (or list breakpoints)\n");
    fmt::print("  h, help          Show this help\n");
    fmt::print("  q, quit          Exit debugger\n");
    fmt::print("\n");
}

DebugCommand InterpreterDebugger::parseCommand(const std::string& input, std::string& arg) const {
    std::istringstream iss(input);
    std::string cmd;
    iss >> cmd;

    // Get remaining input as argument
    std::getline(iss >> std::ws, arg);

    // Parse command
    if (cmd == "c" || cmd == "continue") {
        return DebugCommand::Continue;
    } else if (cmd == "s" || cmd == "step") {
        return DebugCommand::Step;
    } else if (cmd == "n" || cmd == "next") {
        return DebugCommand::Next;
    } else if (cmd == "v" || cmd == "vars") {
        return DebugCommand::Vars;
    } else if (cmd == "p" || cmd == "print") {
        return DebugCommand::Print;
    } else if (cmd == "w" || cmd == "watch") {
        return DebugCommand::Watch;
    } else if (cmd == "b" || cmd == "breakpoint") {
        return DebugCommand::Breakpoint;
    } else if (cmd == "h" || cmd == "help") {
        return DebugCommand::Help;
    } else if (cmd == "q" || cmd == "quit") {
        return DebugCommand::Quit;
    }

    return DebugCommand::Unknown;
}

} // namespace interpreter
} // namespace naab
