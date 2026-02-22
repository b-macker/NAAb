// NAAb REPL - Interactive shell
// Read-Eval-Print Loop with history and multi-line support

#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/interpreter.h"
#include "naab/paths.h"
#include "naab/repl_commands.h"
#include "naab/language_registry.h"
#include "naab/cpp_executor_adapter.h"
#include "naab/js_executor_adapter.h"
#include <fmt/core.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <new>  // For placement new

namespace naab {
namespace repl {

class ReplSession {
public:
    ReplSession()
        : interpreter_(),
          command_handler_(interpreter_),
          line_number_(1),
          in_multiline_(false),
          accumulated_program_("") {
        loadHistory();
    }

    ~ReplSession() {
        saveHistory();
    }

    void run() {
        printWelcome();

        std::string line;
        std::string accumulated_input;

        while (true) {
            // Prompt
            if (in_multiline_) {
                fmt::print("... ");
            } else {
                fmt::print(">>> ");
            }

            // Read line
            if (!std::getline(std::cin, line)) {
                break;  // EOF
            }

            // Add to history
            if (!line.empty() && !in_multiline_) {
                history_.push_back(line);
            }

            // Check for REPL commands
            if (!in_multiline_ && line.length() > 0 && line[0] == ':') {
                handleCommand(line);
                continue;
            }

            // Check for exit
            if (!in_multiline_ && (line == "exit" || line == "quit")) {
                fmt::print("Goodbye!\n");
                break;
            }

            // Accumulate input
            if (in_multiline_) {
                accumulated_input += "\n" + line;
            } else {
                accumulated_input = line;
            }

            // Check if we need more lines (unbalanced braces)
            if (needsMoreInput(accumulated_input)) {
                in_multiline_ = true;
                continue;
            }

            // We have complete input - execute it
            in_multiline_ = false;

            if (!accumulated_input.empty()) {
                executeInput(accumulated_input);
                line_number_++;
            }

            accumulated_input.clear();
        }
    }

private:
    interpreter::Interpreter interpreter_;
    ReplCommandHandler command_handler_;
    std::vector<std::string> history_;
    size_t line_number_;
    bool in_multiline_;
    std::string accumulated_program_;  // All REPL statements accumulated

    void printWelcome() {
        auto& registry = runtime::LanguageRegistry::instance();
        auto languages = registry.supportedLanguages();

        fmt::print("\n");
        fmt::print("╔═══════════════════════════════════════════════════════╗\n");
        fmt::print("║  NAAb Block Assembly Language - Interactive Shell    ║\n");
        fmt::print("║  Version 0.1.0                                        ║\n");
        fmt::print("╚═══════════════════════════════════════════════════════╝\n");
        fmt::print("\n");
        fmt::print("Type :help for help, :exit to quit\n");
        fmt::print("Supported languages: ");
        for (size_t i = 0; i < languages.size(); i++) {
            if (i > 0) fmt::print(", ");
            fmt::print("{}", languages[i]);
        }
        fmt::print("\n");
        fmt::print("24,167 blocks available\n\n");
    }

    void handleCommand(const std::string& cmd) {
        // Delegate to command handler
        bool continue_repl = command_handler_.handleCommand(cmd);

        // Handle special commands that need REPL session state
        if (cmd == ":history") {
            printHistory();
        } else if (cmd == ":reset") {
            fmt::print("[INFO] Resetting interpreter state...\n");
            // Use placement new to reconstruct interpreter (copy assignment is deleted)
            interpreter_.~Interpreter();
            new (&interpreter_) interpreter::Interpreter();
            accumulated_program_.clear();
            line_number_ = 1;
            fmt::print("[SUCCESS] State reset complete\n");
        }

        if (!continue_repl) {
            std::exit(0);
        }
    }

    void printHelp() {
        fmt::print("\n");
        fmt::print("REPL Commands:\n");
        fmt::print("  :help, :h        Show this help message\n");
        fmt::print("  :exit, :quit, :q Exit the REPL\n");
        fmt::print("  :clear, :cls     Clear the screen\n");
        fmt::print("  :history         Show command history\n");
        fmt::print("  :blocks          Show available blocks\n");
        fmt::print("  :reset           Reset interpreter state\n");
        fmt::print("\n");
        fmt::print("Usage:\n");
        fmt::print("  - Enter NAAb expressions or statements\n");
        fmt::print("  - Use {{ }} for multi-line blocks\n");
        fmt::print("  - Variables persist across inputs\n");
        fmt::print("  - Load blocks with: use BLOCK-ID as Name\n");
        fmt::print("\n");
        fmt::print("Examples:\n");
        fmt::print("  >>> let x = 42\n");
        fmt::print("  >>> print(x + 10)\n");
        fmt::print("  >>> use BLOCK-PY-00001 as MathUtil\n");
        fmt::print("  >>> MathUtil()\n");
        fmt::print("\n");
    }

    void printHistory() {
        fmt::print("\nCommand History:\n");
        for (size_t i = 0; i < history_.size(); ++i) {
            fmt::print("  {:3}: {}\n", i + 1, history_[i]);
        }
        fmt::print("\n");
    }

    bool needsMoreInput(const std::string& input) {
        int brace_count = 0;
        int paren_count = 0;
        bool in_string = false;
        char string_delimiter = '\0';

        for (size_t i = 0; i < input.length(); ++i) {
            char c = input[i];

            // Handle strings
            if (c == '"' || c == '\'') {
                if (!in_string) {
                    in_string = true;
                    string_delimiter = c;
                } else if (c == string_delimiter && (i == 0 || input[i-1] != '\\')) {
                    in_string = false;
                }
                continue;
            }

            if (in_string) continue;

            // Count braces and parens
            if (c == '{') brace_count++;
            else if (c == '}') brace_count--;
            else if (c == '(') paren_count++;
            else if (c == ')') paren_count--;
        }

        return brace_count > 0 || paren_count > 0;
    }

    void executeInput(const std::string& input) {
        try {
            // Add to accumulated program
            if (needsWrapping(input)) {
                // It's a statement - add it to the main block
                accumulated_program_ += "    " + input + "\n";
            } else {
                // It's a top-level construct (use, fn, etc)
                // This shouldn't happen in REPL, but handle it
                accumulated_program_ += input + "\n";
            }

            // Build complete program with all accumulated statements
            std::string full_program = accumulated_program_;
            if (!full_program.empty()) {
                full_program = "main {\n" + full_program + "}";
            }

            // Parse and execute complete program
            lexer::Lexer lexer(full_program);
            auto tokens = lexer.tokenize();

            parser::Parser parser(tokens);
            auto program = parser.parseProgram();

            // Execute - this preserves state because the interpreter persists
            interpreter_.execute(*program);

        } catch (const std::exception& e) {
            // On error, remove the last statement from accumulated program
            // Find last statement boundary
            size_t last_newline = accumulated_program_.find_last_of('\n',
                accumulated_program_.length() - 2);
            if (last_newline != std::string::npos) {
                accumulated_program_ = accumulated_program_.substr(0, last_newline + 1);
            } else {
                accumulated_program_.clear();
            }

            fmt::print("Error: {}\n", e.what());
        }
    }

    bool needsWrapping(const std::string& input) {
        // Check if input starts with keywords that indicate a complete program
        std::string trimmed = input;
        // Trim leading whitespace
        size_t start = trimmed.find_first_not_of(" \t\n\r");
        if (start != std::string::npos) {
            trimmed = trimmed.substr(start);
        }

        // If it starts with 'use', 'fn', or 'main', it's a complete program
        if (trimmed.find("use ") == 0) return false;
        if (trimmed.find("fn ") == 0) return false;
        if (trimmed.find("main ") == 0) return false;

        // Otherwise, wrap it
        return true;
    }

    void loadHistory() {
        // Try to load history from file
        std::string history_file = naab::paths::history_file();
        std::ifstream file(history_file);
        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                if (!line.empty()) {
                    history_.push_back(line);
                }
            }
        }
    }

    void saveHistory() {
        // Save last 100 commands
        std::string history_file = naab::paths::history_file();
        std::ofstream file(history_file);
        if (file.is_open()) {
            size_t start = history_.size() > 100 ? history_.size() - 100 : 0;
            for (size_t i = start; i < history_.size(); ++i) {
                file << history_[i] << "\n";
            }
        }
    }
};

void run_repl() {
    ReplSession session;
    session.run();
}

} // namespace repl
} // namespace naab

int main() {
    // Phase 7c: Initialize language executors
    auto& registry = naab::runtime::LanguageRegistry::instance();

    registry.registerExecutor("cpp",
        std::make_unique<naab::runtime::CppExecutorAdapter>());

    registry.registerExecutor("javascript",
        std::make_unique<naab::runtime::JsExecutorAdapter>());

    // Start REPL
    naab::repl::run_repl();
    return 0;
}
