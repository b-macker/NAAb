// NAAb REPL with Readline Support - Enhanced editing with linenoise
// Arrow keys, Ctrl+R search, auto-completion, and full history support

#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/interpreter.h"
#include "naab/paths.h"
#include <fmt/core.h>
#include <string>
#include <vector>
#include <cstring>
#include <new>  // For placement new

// Linenoise for line editing
extern "C" {
#include "../../../external/linenoise.h"
}

namespace naab {
namespace repl {

// Global state for auto-completion
static interpreter::Interpreter* g_interpreter = nullptr;
static std::vector<std::string> g_keywords = {
    "let", "fn", "if", "else", "for", "while", "return",
    "true", "false", "null", "use", "as", "main", "print"
};

// Auto-completion callback
void completion(const char* buf, linenoiseCompletions* lc) {
    std::string input(buf);

    // Extract the last word being typed
    size_t last_space = input.find_last_of(" \t\n");
    std::string prefix = (last_space == std::string::npos) ? input : input.substr(last_space + 1);

    if (prefix.empty()) return;

    // Suggest keywords
    for (const auto& keyword : g_keywords) {
        if (keyword.find(prefix) == 0) {  // Starts with prefix
            linenoiseAddCompletion(lc, (input.substr(0, last_space + 1) + keyword).c_str());
        }
    }

    // TODO: Suggest variable names from environment when accessible
    // TODO: Suggest function names
}

// History hints callback
char* hints(const char* buf, int* color, int* bold) {
    // Could show hints like "press Tab for completion"
    return nullptr;
}

class ReadlineReplSession {
public:
    ReadlineReplSession()
        : interpreter_(),
          in_multiline_(false),
          statement_count_(0) {

        g_interpreter = &interpreter_;

        // Configure linenoise
        linenoiseSetMultiLine(1);  // Enable multi-line mode
        linenoiseSetCompletionCallback(completion);
        linenoiseSetHintsCallback(hints);

        // Load history
        linenoiseHistoryLoad(naab::paths::history_file().c_str());
        linenoiseHistorySetMaxLen(1000);
    }

    ~ReadlineReplSession() {
        // Save history
        linenoiseHistorySave(naab::paths::history_file().c_str());
        g_interpreter = nullptr;
    }

    void run() {
        printWelcome();

        std::string accumulated_input;
        char* line_ptr;

        while (true) {
            // Prompt
            const char* prompt = in_multiline_ ? "... " : ">>> ";

            // Read line with linenoise (supports arrow keys, Ctrl+R, etc.)
            line_ptr = linenoise(prompt);

            if (line_ptr == nullptr) {
                // EOF (Ctrl+D)
                printStats();
                fmt::print("\nGoodbye!\n");
                break;
            }

            std::string line(line_ptr);

            // Add to history (linenoise handles deduplication)
            if (!line.empty() && !in_multiline_) {
                linenoiseHistoryAdd(line.c_str());
            }

            // Free the line buffer
            linenoiseFree(line_ptr);

            // Check for REPL commands
            if (!in_multiline_ && !line.empty() && line[0] == ':') {
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
            }

            accumulated_input.clear();
        }
    }

private:
    interpreter::Interpreter interpreter_;
    bool in_multiline_;
    size_t statement_count_;
    double total_exec_time_ms_;

    void printWelcome() {
        fmt::print("\n");
        fmt::print("╔═══════════════════════════════════════════════════════╗\n");
        fmt::print("║  NAAb REPL - With Readline Support (linenoise)       ║\n");
        fmt::print("║  Version 0.1.0                                        ║\n");
        fmt::print("╚═══════════════════════════════════════════════════════╝\n");
        fmt::print("\n");
        fmt::print("Features:\n");
        fmt::print("  • Arrow keys for navigation and history\n");
        fmt::print("  • Ctrl+R for reverse search\n");
        fmt::print("  • Tab for auto-completion\n");
        fmt::print("  • Ctrl+A/E for line start/end\n");
        fmt::print("  • Ctrl+U to clear line\n");
        fmt::print("\n");
        fmt::print("Type :help for help, :exit to quit\n");
        fmt::print("24,167 blocks available\n\n");
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
            linenoiseClearScreen();
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
        fmt::print("Keyboard Shortcuts:\n");
        fmt::print("  Up/Down          Navigate history\n");
        fmt::print("  Left/Right       Move cursor\n");
        fmt::print("  Ctrl+A           Move to line start\n");
        fmt::print("  Ctrl+E           Move to line end\n");
        fmt::print("  Ctrl+U           Clear line\n");
        fmt::print("  Ctrl+K           Delete to end of line\n");
        fmt::print("  Ctrl+W           Delete previous word\n");
        fmt::print("  Ctrl+R           Reverse search history\n");
        fmt::print("  Tab              Auto-completion\n");
        fmt::print("  Ctrl+D           Exit (EOF)\n");
        fmt::print("\n");
    }

    void printHistory() {
        // Linenoise doesn't expose history directly, so we note this
        fmt::print("\nHistory is stored in ~/.naab_history\n");
        fmt::print("Use Up/Down arrows to navigate history\n");
        fmt::print("Use Ctrl+R for reverse search\n\n");
    }

    void printStats() {
        fmt::print("\n");
        fmt::print("Session Statistics:\n");
        fmt::print("  Statements executed: {}\n", statement_count_);
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

    void executeInputIncremental(const std::string& input) {
        try {
            // Wrap in minimal context for parsing
            std::string wrapped = "main { " + input + " }";

            lexer::Lexer lexer(wrapped);
            auto tokens = lexer.tokenize();

            parser::Parser parser(tokens);
            auto program = parser.parseProgram();

            // Execute the program
            interpreter_.execute(*program);

            statement_count_++;

        } catch (const std::exception& e) {
            fmt::print("Error: {}\n", e.what());
        }
    }
};

void run_repl_readline() {
    ReadlineReplSession session;
    session.run();
}

} // namespace repl
} // namespace naab

int main() {
    naab::repl::run_repl_readline();
    return 0;
}
