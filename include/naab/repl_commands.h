#pragma once

// NAAb REPL Commands
// Handles REPL command processing for block management and utilities

#include "naab/interpreter.h"
#include <string>
#include <vector>

namespace naab {
namespace repl {

// REPL Command Handler
class ReplCommandHandler {
public:
    explicit ReplCommandHandler(interpreter::Interpreter& interp);
    ~ReplCommandHandler() = default;

    // Process a REPL command (starts with ':')
    bool handleCommand(const std::string& cmd_line);

    // Get list of available commands
    std::vector<std::string> getAvailableCommands() const;

private:
    interpreter::Interpreter& interpreter_;

    // Command handlers
    void handleHelp();
    void handleExit();
    void handleClear();
    void handleHistory(const std::vector<std::string>& history);
    void handleReset();

    // Phase 7b: Block management commands
    void handleLoad(const std::string& cmd_line);
    void handleBlocks();
    void handleInfo(const std::string& alias);
    void handleReload(const std::string& alias);
    void handleUnload(const std::string& alias);
    void handleLanguages();

    // Phase 4: Debugger commands
    void handleBreak(const std::string& location, const std::string& condition = "");
    void handleClearBreakpoint(int id);
    void handleListBreakpoints();
    void handleContinue();
    void handleNext();
    void handleStep();
    void handleFinish();
    void handleStack();
    void handleLocals();
    void handleVar(const std::string& name);
    void handleWatch(const std::string& expression);
    void handleUnwatch(int id);
    void handleWatches();

    // Helper methods
    std::vector<std::string> parseCommand(const std::string& cmd_line);
    void printCommandHelp();
};

} // namespace repl
} // namespace naab

