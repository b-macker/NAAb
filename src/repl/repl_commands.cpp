// NAAb REPL Commands Implementation
// Block management and utility commands for the REPL

#include "naab/repl_commands.h"
#include "naab/language_registry.h"
#include "naab/block_registry.h"
#include "naab/block_loader.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include <fmt/core.h>
#include <sstream>
#include <algorithm>
#include <cstdlib>

namespace naab {
namespace repl {

ReplCommandHandler::ReplCommandHandler(interpreter::Interpreter& interp)
    : interpreter_(interp) {
}

bool ReplCommandHandler::handleCommand(const std::string& cmd_line) {
    auto parts = parseCommand(cmd_line);
    if (parts.empty()) {
        return true;  // Empty command, continue REPL
    }

    const std::string& cmd = parts[0];

    // Basic commands
    if (cmd == ":help" || cmd == ":h") {
        handleHelp();
        return true;
    }

    if (cmd == ":exit" || cmd == ":quit" || cmd == ":q") {
        handleExit();
        return false;  // Exit REPL
    }

    if (cmd == ":clear" || cmd == ":cls") {
        handleClear();
        return true;
    }

    if (cmd == ":reset") {
        handleReset();
        return true;
    }

    // Phase 7b: Block management commands
    if (cmd == ":load") {
        handleLoad(cmd_line);
        return true;
    }

    if (cmd == ":blocks") {
        handleBlocks();
        return true;
    }

    if (cmd == ":info") {
        if (parts.size() < 2) {
            fmt::print("[ERROR] Usage: :info <alias>\n");
        } else {
            handleInfo(parts[1]);
        }
        return true;
    }

    if (cmd == ":reload") {
        if (parts.size() < 2) {
            fmt::print("[ERROR] Usage: :reload <alias>\n");
        } else {
            handleReload(parts[1]);
        }
        return true;
    }

    if (cmd == ":unload") {
        if (parts.size() < 2) {
            fmt::print("[ERROR] Usage: :unload <alias>\n");
        } else {
            handleUnload(parts[1]);
        }
        return true;
    }

    if (cmd == ":languages") {
        handleLanguages();
        return true;
    }

    // Phase 4: Debugger commands
    if (cmd == ":break" || cmd == ":b") {
        if (parts.size() < 2) {
            fmt::print("[ERROR] Usage: :break <location> [condition]\n");
        } else {
            std::string location = parts[1];
            std::string condition = "";
            if (parts.size() > 2) {
                // Join remaining parts as condition
                for (size_t i = 2; i < parts.size(); i++) {
                    if (!condition.empty()) condition += " ";
                    condition += parts[i];
                }
            }
            handleBreak(location, condition);
        }
        return true;
    }

    if (cmd == ":clear") {
        if (parts.size() < 2) {
            handleClear();  // Clear screen if no ID provided
        } else {
            try {
                int id = std::stoi(parts[1]);
                handleClearBreakpoint(id);
            } catch (...) {
                handleClear();  // Fall back to clear screen
            }
        }
        return true;
    }

    if (cmd == ":list") {
        handleListBreakpoints();
        return true;
    }

    if (cmd == ":continue" || cmd == ":c") {
        handleContinue();
        return true;
    }

    if (cmd == ":next" || cmd == ":n") {
        handleNext();
        return true;
    }

    if (cmd == ":step" || cmd == ":s") {
        handleStep();
        return true;
    }

    if (cmd == ":finish" || cmd == ":f") {
        handleFinish();
        return true;
    }

    if (cmd == ":stack" || cmd == ":bt") {
        handleStack();
        return true;
    }

    if (cmd == ":locals") {
        handleLocals();
        return true;
    }

    if (cmd == ":var") {
        if (parts.size() < 2) {
            fmt::print("[ERROR] Usage: :var <variable_name>\n");
        } else {
            handleVar(parts[1]);
        }
        return true;
    }

    if (cmd == ":watch") {
        if (parts.size() < 2) {
            fmt::print("[ERROR] Usage: :watch <expression>\n");
        } else {
            std::string expression = "";
            for (size_t i = 1; i < parts.size(); i++) {
                if (!expression.empty()) expression += " ";
                expression += parts[i];
            }
            handleWatch(expression);
        }
        return true;
    }

    if (cmd == ":unwatch") {
        if (parts.size() < 2) {
            fmt::print("[ERROR] Usage: :unwatch <id>\n");
        } else {
            try {
                int id = std::stoi(parts[1]);
                handleUnwatch(id);
            } catch (...) {
                fmt::print("[ERROR] Invalid watch ID\n");
            }
        }
        return true;
    }

    if (cmd == ":watches") {
        handleWatches();
        return true;
    }

    // Unknown command
    fmt::print("[ERROR] Unknown command: {}\n", cmd);
    fmt::print("        Type :help for available commands\n");
    return true;
}

std::vector<std::string> ReplCommandHandler::parseCommand(const std::string& cmd_line) {
    std::vector<std::string> parts;
    std::istringstream iss(cmd_line);
    std::string token;

    while (iss >> token) {
        parts.push_back(token);
    }

    return parts;
}

void ReplCommandHandler::handleHelp() {
    fmt::print("\n");
    fmt::print("═══════════════════════════════════════════════════════════\n");
    fmt::print("  NAAb REPL Commands\n");
    fmt::print("═══════════════════════════════════════════════════════════\n");
    fmt::print("\n");
    fmt::print("General:\n");
    fmt::print("  :help, :h            Show this help message\n");
    fmt::print("  :exit, :quit, :q     Exit the REPL\n");
    fmt::print("  :clear, :cls         Clear the screen\n");
    fmt::print("  :reset               Reset interpreter state\n");
    fmt::print("\n");
    fmt::print("Block Management:\n");
    fmt::print("  :load <id> as <name> Load a block with alias\n");
    fmt::print("  :blocks              List all loaded blocks\n");
    fmt::print("  :info <name>         Show block information\n");
    fmt::print("  :reload <name>       Reload a block\n");
    fmt::print("  :unload <name>       Unload a block\n");
    fmt::print("  :languages           Show supported languages\n");
    fmt::print("\n");
    fmt::print("Debugging:\n");
    fmt::print("  :break <loc> [cond]  Set breakpoint (e.g., :break main.naab:15)\n");
    fmt::print("  :b <loc>             Short for :break\n");
    fmt::print("  :clear <id>          Clear breakpoint by ID\n");
    fmt::print("  :list                List all breakpoints\n");
    fmt::print("  :continue, :c        Continue execution\n");
    fmt::print("  :next, :n            Step over (next line)\n");
    fmt::print("  :step, :s            Step into function\n");
    fmt::print("  :finish, :f          Step out of function\n");
    fmt::print("  :stack, :bt          Show call stack\n");
    fmt::print("  :locals              List local variables\n");
    fmt::print("  :var <name>          Inspect variable value\n");
    fmt::print("  :watch <expr>        Add watch expression\n");
    fmt::print("  :unwatch <id>        Remove watch expression\n");
    fmt::print("  :watches             List watch expressions\n");
    fmt::print("\n");
    fmt::print("Usage:\n");
    fmt::print("  - Enter NAAb expressions or statements\n");
    fmt::print("  - Use {{ }} for multi-line blocks\n");
    fmt::print("  - Variables persist across inputs\n");
    fmt::print("\n");
    fmt::print("Examples:\n");
    fmt::print("  >>> let x = 42\n");
    fmt::print("  >>> print(x + 10)\n");
    fmt::print("  >>> use BLOCK-CPP-MATH as math\n");
    fmt::print("  >>> math.add(5, 10)\n");
    fmt::print("  >>> :load BLOCK-JS-UTIL as util\n");
    fmt::print("  >>> :info math\n");
    fmt::print("  >>> :break my_function\n");
    fmt::print("  >>> :watch x > 100\n");
    fmt::print("\n");
}

void ReplCommandHandler::handleExit() {
    fmt::print("Goodbye!\n");
    std::exit(0);
}

void ReplCommandHandler::handleClear() {
    // ANSI escape codes to clear screen and move cursor to home
    fmt::print("\033[2J\033[H");
    fmt::print("\n");
    fmt::print("NAAb v0.1.0 - Block Assembly Language REPL\n");
    fmt::print("Type :help for available commands\n");
    fmt::print("\n");
}

void ReplCommandHandler::handleReset() {
    fmt::print("[INFO] Resetting interpreter state...\n");
    // Note: Can't actually reset interpreter from here since we hold a reference
    // This would need to be done at ReplSession level
    fmt::print("[WARN] Full reset requires restarting the REPL\n");
    fmt::print("       Use 'exit' and restart naab-lang\n");
}

// Phase 7b: Block management commands

void ReplCommandHandler::handleLoad(const std::string& cmd_line) {
    // Parse: :load BLOCK-ID as alias
    auto parts = parseCommand(cmd_line);

    if (parts.size() < 4 || parts[2] != "as") {
        fmt::print("[ERROR] Usage: :load <block-id> as <alias>\n");
        fmt::print("        Example: :load BLOCK-CPP-MATH as math\n");
        return;
    }

    std::string block_id = parts[1];
    std::string alias = parts[3];

    fmt::print("[INFO] Loading block {} as '{}'...\n", block_id, alias);

    // Create a use statement and execute it via interpreter
    std::string use_stmt = fmt::format("use {} as {}", block_id, alias);

    try {
        // Parse and execute the use statement
        lexer::Lexer lexer(use_stmt);
        auto tokens = lexer.tokenize();

        parser::Parser parser(tokens);
        auto program = parser.parseProgram();

        interpreter_.execute(*program);

        fmt::print("[SUCCESS] Block loaded successfully\n");

    } catch (const std::exception& e) {
        fmt::print("[ERROR] Failed to load block: {}\n", e.what());
    }
}

void ReplCommandHandler::handleBlocks() {
    fmt::print("\n");
    fmt::print("═══════════════════════════════════════════════════════════\n");
    fmt::print("  Available Blocks\n");
    fmt::print("═══════════════════════════════════════════════════════════\n");
    fmt::print("\n");

    // Phase 8: Use BlockRegistry to list all available blocks
    auto& registry = runtime::BlockRegistry::instance();
    auto langs = registry.supportedLanguages();

    fmt::print("Total blocks: {}\n\n", registry.blockCount());

    // Group by language
    for (const auto& lang : langs) {
        auto lang_blocks = registry.listBlocksByLanguage(lang);
        if (lang_blocks.empty()) continue;

        fmt::print("  [{}] ({} blocks)\n", lang, lang_blocks.size());

        // Show first 10 blocks for each language
        size_t show_count = std::min(size_t(10), lang_blocks.size());
        for (size_t i = 0; i < show_count; i++) {
            fmt::print("    • {}\n", lang_blocks[i]);
        }

        if (lang_blocks.size() > show_count) {
            fmt::print("    ... and {} more\n", lang_blocks.size() - show_count);
        }
        fmt::print("\n");
    }

    fmt::print("Use ':load <block-id> as <alias>' to load a block\n");
    fmt::print("Use ':languages' to see supported languages\n");
    fmt::print("\n");
}

void ReplCommandHandler::handleInfo(const std::string& alias) {
    fmt::print("\n");
    fmt::print("Block Information: {}\n", alias);
    fmt::print("─────────────────────────────────────────────────────────\n");
    fmt::print("\n");

    // Get block information from interpreter environment
    // Note: This requires adding a method to Interpreter to get block info
    fmt::print("[INFO] Block info functionality requires interpreter API extension\n");
    fmt::print("       Alias: {}\n", alias);
    fmt::print("       Status: Requires interpreter.getBlockInfo() method\n");
    fmt::print("\n");
}

void ReplCommandHandler::handleReload(const std::string& alias) {
    fmt::print("[INFO] Reloading block '{}'...\n", alias);

    // Reloading requires:
    // 1. Getting the original block ID from the alias
    // 2. Unloading the current block
    // 3. Loading it again with the same alias

    fmt::print("[INFO] Reload functionality requires interpreter API extension\n");
    fmt::print("       For now, use :unload then :load\n");
}

void ReplCommandHandler::handleUnload(const std::string& alias) {
    fmt::print("[INFO] Unloading block '{}'...\n", alias);

    // Unloading requires removing the variable from interpreter environment
    // Note: This requires adding a method to Interpreter to remove variables
    fmt::print("[INFO] Unload functionality requires interpreter API extension\n");
    fmt::print("       Variable '{}' will be undefined on next reset\n", alias);
}

void ReplCommandHandler::handleLanguages() {
    auto& registry = runtime::LanguageRegistry::instance();
    auto languages = registry.supportedLanguages();

    fmt::print("\n");
    fmt::print("═══════════════════════════════════════════════════════════\n");
    fmt::print("  Supported Languages\n");
    fmt::print("═══════════════════════════════════════════════════════════\n");
    fmt::print("\n");

    if (languages.empty()) {
        fmt::print("  No languages registered.\n");
        fmt::print("  Register executors in main() to enable languages.\n");
    } else {
        for (const auto& lang : languages) {
            auto* executor = registry.getExecutor(lang);
            std::string status = executor && executor->isInitialized() ? "✓ ready" : "✗ not initialized";
            fmt::print("  • {:12} {}\n", lang, status);
        }
    }

    fmt::print("\n");
    fmt::print("Use 'use BLOCK-<LANG>-<ID> as name' to load blocks\n");
    fmt::print("\n");
}

// ============================================================================
// Phase 4: Debugger Commands
// ============================================================================

void ReplCommandHandler::handleBreak(const std::string& location, const std::string& condition) {
    auto debugger = interpreter_.getDebugger();
    if (!debugger) {
        fmt::print("[ERROR] Debugger not initialized\n");
        fmt::print("        Enable debugging to use breakpoints\n");
        return;
    }

    int id = debugger->setBreakpoint(location, condition);

    if (condition.empty()) {
        fmt::print("[SUCCESS] Breakpoint {} set at {}\n", id, location);
    } else {
        fmt::print("[SUCCESS] Breakpoint {} set at {} (condition: {})\n", id, location, condition);
    }
}

void ReplCommandHandler::handleClearBreakpoint(int id) {
    auto debugger = interpreter_.getDebugger();
    if (!debugger) {
        fmt::print("[ERROR] Debugger not initialized\n");
        return;
    }

    if (debugger->clearBreakpoint(id)) {
        fmt::print("[SUCCESS] Breakpoint {} cleared\n", id);
    } else {
        fmt::print("[ERROR] Breakpoint {} not found\n", id);
    }
}

void ReplCommandHandler::handleListBreakpoints() {
    auto debugger = interpreter_.getDebugger();
    if (!debugger) {
        fmt::print("[ERROR] Debugger not initialized\n");
        return;
    }

    auto breakpoints = debugger->listBreakpoints();

    if (breakpoints.empty()) {
        fmt::print("No breakpoints set.\n");
        return;
    }

    fmt::print("\n");
    fmt::print("Breakpoints:\n");
    fmt::print("═══════════════════════════════════════════════════════════\n");
    for (const auto& bp : breakpoints) {
        std::string status = bp.enabled ? "enabled" : "disabled";
        fmt::print("  [{}] {} ({}) - hits: {}\n", bp.id, bp.location, status, bp.hit_count);
        if (!bp.condition.empty()) {
            fmt::print("      Condition: {}\n", bp.condition);
        }
    }
    fmt::print("\n");
}

void ReplCommandHandler::handleContinue() {
    auto debugger = interpreter_.getDebugger();
    if (!debugger) {
        fmt::print("[ERROR] Debugger not initialized\n");
        return;
    }

    debugger->resume();
    fmt::print("[DEBUG] Continuing execution...\n");
}

void ReplCommandHandler::handleNext() {
    auto debugger = interpreter_.getDebugger();
    if (!debugger) {
        fmt::print("[ERROR] Debugger not initialized\n");
        return;
    }

    debugger->step(debugger::StepMode::OVER);
    fmt::print("[DEBUG] Stepping over...\n");
}

void ReplCommandHandler::handleStep() {
    auto debugger = interpreter_.getDebugger();
    if (!debugger) {
        fmt::print("[ERROR] Debugger not initialized\n");
        return;
    }

    debugger->step(debugger::StepMode::INTO);
    fmt::print("[DEBUG] Stepping into...\n");
}

void ReplCommandHandler::handleFinish() {
    auto debugger = interpreter_.getDebugger();
    if (!debugger) {
        fmt::print("[ERROR] Debugger not initialized\n");
        return;
    }

    debugger->step(debugger::StepMode::OUT);
    fmt::print("[DEBUG] Stepping out...\n");
}

void ReplCommandHandler::handleStack() {
    auto debugger = interpreter_.getDebugger();
    if (!debugger) {
        fmt::print("[ERROR] Debugger not initialized\n");
        return;
    }

    auto stack = debugger->getCallStack();

    if (stack.empty()) {
        fmt::print("Call stack is empty.\n");
        return;
    }

    fmt::print("\n");
    fmt::print("Call Stack:\n");
    fmt::print("═══════════════════════════════════════════════════════════\n");
    for (size_t i = 0; i < stack.size(); i++) {
        const auto& frame = stack[i];
        fmt::print("  #{} {} at {}\n", i, frame.function_name, frame.source_location);
    }
    fmt::print("\n");
}

void ReplCommandHandler::handleLocals() {
    auto debugger = interpreter_.getDebugger();
    if (!debugger) {
        fmt::print("[ERROR] Debugger not initialized\n");
        return;
    }

    auto locals = debugger->listLocalVariables();

    if (locals.empty()) {
        fmt::print("No local variables in current scope.\n");
        return;
    }

    fmt::print("\n");
    fmt::print("Local Variables:\n");
    fmt::print("═══════════════════════════════════════════════════════════\n");
    for (const auto& [name, value] : locals) {
        fmt::print("  {} = {}\n", name, value->toString());
    }
    fmt::print("\n");
}

void ReplCommandHandler::handleVar(const std::string& name) {
    auto debugger = interpreter_.getDebugger();
    if (!debugger) {
        fmt::print("[ERROR] Debugger not initialized\n");
        return;
    }

    auto value = debugger->inspectVariable(name);

    if (!value) {
        fmt::print("[ERROR] Variable '{}' not found\n", name);
        return;
    }

    fmt::print("{} = {}\n", name, value->toString());
}

void ReplCommandHandler::handleWatch(const std::string& expression) {
    auto debugger = interpreter_.getDebugger();
    if (!debugger) {
        fmt::print("[ERROR] Debugger not initialized\n");
        return;
    }

    int id = debugger->addWatch(expression);
    fmt::print("[SUCCESS] Watch {} added: {}\n", id, expression);
}

void ReplCommandHandler::handleUnwatch(int id) {
    auto debugger = interpreter_.getDebugger();
    if (!debugger) {
        fmt::print("[ERROR] Debugger not initialized\n");
        return;
    }

    if (debugger->removeWatch(id)) {
        fmt::print("[SUCCESS] Watch {} removed\n", id);
    } else {
        fmt::print("[ERROR] Watch {} not found\n", id);
    }
}

void ReplCommandHandler::handleWatches() {
    auto debugger = interpreter_.getDebugger();
    if (!debugger) {
        fmt::print("[ERROR] Debugger not initialized\n");
        return;
    }

    auto results = debugger->evaluateWatches();

    if (results.empty()) {
        fmt::print("No watches set.\n");
        return;
    }

    fmt::print("\n");
    fmt::print("Watch Expressions:\n");
    fmt::print("═══════════════════════════════════════════════════════════\n");
    for (const auto& result : results) {
        if (result.error.empty() && result.value) {
            fmt::print("  [{}] {} = {}\n", result.id, result.expression, result.value->toString());
        } else {
            fmt::print("  [{}] {} = ERROR: {}\n", result.id, result.expression, result.error);
        }
    }
    fmt::print("\n");
}

std::vector<std::string> ReplCommandHandler::getAvailableCommands() const {
    return {
        ":help", ":h",
        ":exit", ":quit", ":q",
        ":clear", ":cls",
        ":reset",
        ":load",
        ":blocks",
        ":info",
        ":reload",
        ":unload",
        ":languages",
        ":break", ":b",
        ":list",
        ":continue", ":c",
        ":next", ":n",
        ":step", ":s",
        ":finish", ":f",
        ":stack", ":bt",
        ":locals",
        ":var",
        ":watch",
        ":unwatch",
        ":watches"
    };
}

} // namespace repl
} // namespace naab
