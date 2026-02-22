// NAAb REPL - Optimized with Incremental Execution
// O(1) per statement instead of O(n) by only executing new statements

#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/interpreter.h"
#include "naab/paths.h"
#include <fmt/core.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <chrono>
#include <new>  // For placement new

namespace naab {
namespace repl {

class OptimizedReplSession {
public:
    OptimizedReplSession()
        : interpreter_(),
          line_number_(1),
          in_multiline_(false),
          statement_count_(0),
          total_exec_time_ms_(0) {
        loadHistory();
    }

    ~OptimizedReplSession() {
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
                printStats();
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
                executeInputIncremental(accumulated_input);
                line_number_++;
            }

            accumulated_input.clear();
        }
    }

private:
    interpreter::Interpreter interpreter_;
    std::vector<std::string> history_;
    size_t line_number_;
    bool in_multiline_;
    size_t statement_count_;
    double total_exec_time_ms_;

    void printWelcome() {
        fmt::print("\n");
        fmt::print("╔═══════════════════════════════════════════════════════╗\n");
        fmt::print("║  NAAb REPL - Optimized with Incremental Execution    ║\n");
        fmt::print("║  Version 0.1.0                                        ║\n");
        fmt::print("╚═══════════════════════════════════════════════════════╝\n");
        fmt::print("\n");
        fmt::print("Type :help for help, :exit to quit\n");
        fmt::print("24,167 blocks available\n");
        fmt::print("Performance: O(1) incremental execution enabled\n\n");
    }

    void handleCommand(const std::string& cmd) {
        if (cmd == ":help" || cmd == ":h") {
            printHelp();
        } else if (cmd == ":exit" || cmd == ":quit" || cmd == ":q") {
            printStats();
            fmt::print("Goodbye!\n");
            std::exit(0);
        } else if (cmd == ":clear" || cmd == ":cls") {
            // Clear screen
            fmt::print("\033[2J\033[H");
            printWelcome();
        } else if (cmd == ":history") {
            printHistory();
        } else if (cmd == ":blocks") {
            fmt::print("24,167 blocks available in registry\n");
            fmt::print("Use 'use BLOCK-ID as Name' to load a block\n");
        } else if (cmd == ":reset") {
            fmt::print("Resetting interpreter state...\n");
            // Use placement new to reconstruct interpreter (copy assignment is deleted)
            interpreter_.~Interpreter();
            new (&interpreter_) interpreter::Interpreter();
            statement_count_ = 0;
            total_exec_time_ms_ = 0;
            line_number_ = 1;
            fmt::print("State reset complete\n");
        } else if (cmd == ":stats") {
            printStats();
        } else {
            fmt::print("Unknown command: {}\n", cmd);
            fmt::print("Type :help for available commands\n");
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
        fmt::print("  :stats           Show performance statistics\n");
        fmt::print("\n");
        fmt::print("Usage:\n");
        fmt::print("  - Enter NAAb expressions or statements\n");
        fmt::print("  - Use {{ }} for multi-line blocks\n");
        fmt::print("  - Variables persist across inputs\n");
        fmt::print("  - Load blocks with: use BLOCK-ID as Name\n");
        fmt::print("\n");
        fmt::print("Optimizations:\n");
        fmt::print("  - Incremental execution: O(1) per statement\n");
        fmt::print("  - No re-parsing of previous statements\n");
        fmt::print("  - State preserved across inputs\n");
        fmt::print("\n");
    }

    void printHistory() {
        fmt::print("\nCommand History:\n");
        for (size_t i = 0; i < history_.size(); ++i) {
            fmt::print("  {:3}: {}\n", i + 1, history_[i]);
        }
        fmt::print("\n");
    }

    void printStats() {
        fmt::print("\n");
        fmt::print("Performance Statistics:\n");
        fmt::print("  Statements executed: {}\n", statement_count_);
        fmt::print("  Total execution time: {:.2f}ms\n", total_exec_time_ms_);
        if (statement_count_ > 0) {
            fmt::print("  Average per statement: {:.3f}ms\n",
                total_exec_time_ms_ / statement_count_);
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

    // OPTIMIZED: Only execute new statements, don't re-execute old ones
    void executeInputIncremental(const std::string& input) {
        auto start_time = std::chrono::high_resolution_clock::now();

        try {
            // Check if this is a top-level construct (use, fn) or a statement
            if (isTopLevelConstruct(input)) {
                // Parse as a complete program with this single construct
                std::string wrapped = input;
                if (input.find("use ") == 0) {
                    // use statements need wrapping
                    wrapped = input + "\nmain {}";
                }

                lexer::Lexer lexer(wrapped);
                auto tokens = lexer.tokenize();

                parser::Parser parser(tokens);
                auto program = parser.parseProgram();

                interpreter_.execute(*program);
            } else {
                // It's a statement - parse and execute directly
                // Wrap in a minimal context for parsing
                std::string wrapped = "main { " + input + " }";

                lexer::Lexer lexer(wrapped);
                auto tokens = lexer.tokenize();

                parser::Parser parser(tokens);
                auto program = parser.parseProgram();

                // Execute the program (which contains only the new statement)
                interpreter_.execute(*program);
            }

            statement_count_++;

            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                end_time - start_time);
            total_exec_time_ms_ += duration.count() / 1000.0;

        } catch (const std::exception& e) {
            fmt::print("Error: {}\n", e.what());

            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                end_time - start_time);
            total_exec_time_ms_ += duration.count() / 1000.0;
        }
    }

    bool isTopLevelConstruct(const std::string& input) {
        std::string trimmed = input;
        // Trim leading whitespace
        size_t start = trimmed.find_first_not_of(" \t\n\r");
        if (start != std::string::npos) {
            trimmed = trimmed.substr(start);
        }

        // Check for top-level keywords
        return trimmed.find("use ") == 0 ||
               trimmed.find("fn ") == 0 ||
               trimmed.find("main ") == 0;
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

void run_repl_optimized() {
    OptimizedReplSession session;
    session.run();
}

} // namespace repl
} // namespace naab

int main() {
    naab::repl::run_repl_optimized();
    return 0;
}
