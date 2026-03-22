//
// NAAb REPL — Interactive Read-Eval-Print Loop
//

#include "repl.h"
#include "naab/interpreter.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include <iostream>
#include <string>
#include <fstream>

namespace naab {
namespace repl {

// Check if input has unclosed braces/brackets/parens (needs more lines)
static bool isIncomplete(const std::string& input) {
    int braces = 0, brackets = 0, parens = 0;
    bool in_string = false;
    char string_char = 0;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        // Handle escape sequences
        if (in_string && c == '\\' && i + 1 < input.size()) {
            ++i;
            continue;
        }

        // Handle string boundaries
        if ((c == '"' || c == '\'') && !in_string) {
            in_string = true;
            string_char = c;
            continue;
        }
        if (c == string_char && in_string) {
            in_string = false;
            continue;
        }

        if (in_string) continue;

        // Handle single-line comments
        if (c == '/' && i + 1 < input.size() && input[i + 1] == '/') break;

        if (c == '{') braces++;
        else if (c == '}') braces--;
        else if (c == '[') brackets++;
        else if (c == ']') brackets--;
        else if (c == '(') parens++;
        else if (c == ')') parens--;
    }

    return braces > 0 || brackets > 0 || parens > 0;
}

// Save a line to history file
static void saveHistory(const std::string& line) {
    std::string home = getenv("HOME") ? getenv("HOME") : ".";
    std::string history_path = home + "/.naab_history";
    std::ofstream file(history_path, std::ios::app);
    if (file.is_open()) {
        file << line << "\n";
    }
}

int run(bool /*no_governance*/) {
    std::cout << "NAAb REPL v0.6.0 — Type .help for help, .exit to quit\n";
    std::cout << ">>> ";
    std::cout.flush();

    // Create persistent interpreter with shared environment
    auto interpreter = std::make_unique<interpreter::Interpreter>();
    interpreter->setReplMode(true);

    // Keep parsed programs alive so AST nodes (function bodies) aren't freed
    std::vector<std::unique_ptr<ast::Program>> program_store;

    std::string input;
    std::string accumulated;
    bool multiline = false;

    while (std::getline(std::cin, input)) {
        // Handle meta-commands
        if (!multiline && !input.empty() && input[0] == '.') {
            if (input == ".exit" || input == ".quit" || input == ".q") {
                std::cout << "Bye!\n";
                return 0;
            }
            if (input == ".help" || input == ".h") {
                std::cout << "NAAb REPL Commands:\n";
                std::cout << "  .help    Show this help\n";
                std::cout << "  .clear   Clear the environment\n";
                std::cout << "  .exit    Exit the REPL\n";
                std::cout << "\n";
                std::cout << "Enter expressions or statements. Results are printed automatically.\n";
                std::cout << "Multi-line input: open braces/brackets continue to next line.\n";
                std::cout << ">>> ";
                std::cout.flush();
                continue;
            }
            if (input == ".clear") {
                interpreter = std::make_unique<interpreter::Interpreter>();
                interpreter->setReplMode(true);
                std::cout << "Environment cleared.\n";
                std::cout << ">>> ";
                std::cout.flush();
                continue;
            }
            std::cout << "Unknown command: " << input << " (try .help)\n";
            std::cout << ">>> ";
            std::cout.flush();
            continue;
        }

        // Accumulate input
        if (multiline) {
            accumulated += "\n" + input;
        } else {
            accumulated = input;
        }

        // Check if we need more input
        if (isIncomplete(accumulated)) {
            multiline = true;
            std::cout << "... ";
            std::cout.flush();
            continue;
        }

        multiline = false;
        std::string code = accumulated;
        accumulated.clear();

        // Skip empty lines
        if (code.empty() || code.find_first_not_of(" \t\n") == std::string::npos) {
            std::cout << ">>> ";
            std::cout.flush();
            continue;
        }

        saveHistory(code);

        // Try to parse and evaluate
        try {
            // Wrap in main{} for the interpreter (REPL lines are top-level)
            // But first try as expression (for things like "3 + 4")
            std::string wrapped;

            // Heuristic: if it starts with let/fn/struct/enum/import/use/if/for/while/print,
            // treat as statement. Otherwise try as expression first.
            std::string trimmed = code;
            trimmed.erase(0, trimmed.find_first_not_of(" \t"));

            // Check if this is a top-level declaration (fn, struct, enum, use, import)
            // These go OUTSIDE main{} so the interpreter registers them properly.
            bool is_toplevel = false;
            static const std::string toplevel_prefixes[] = {
                "fn ", "struct ", "enum ", "import ", "use ",
                "export "
            };
            for (const auto& prefix : toplevel_prefixes) {
                if (trimmed.substr(0, prefix.size()) == prefix) {
                    is_toplevel = true;
                    break;
                }
            }

            bool is_statement = false;
            static const std::string stmt_prefixes[] = {
                "let ", "if ", "if(", "for ", "for(", "while ", "while(",
                "print(", "print ", "return ", "break", "continue",
                "try ", "try{", "match "
            };
            if (!is_toplevel) {
                for (const auto& prefix : stmt_prefixes) {
                    if (trimmed.substr(0, prefix.size()) == prefix) {
                        is_statement = true;
                        break;
                    }
                }
            }

            if (is_toplevel) {
                // Top-level: fn, struct, enum, use, import
                wrapped = code + "\nmain {}\n";
            } else if (is_statement) {
                wrapped = "main {\n" + code + "\n}\n";
            } else {
                // Try as expression — wrap in print()
                wrapped = "main {\nlet __repl_result__ = " + code + "\nprint(__repl_result__)\n}\n";
            }

            // Parse
            lexer::Lexer lex(wrapped);
            auto tokens = lex.tokenize();
            parser::Parser par(tokens);
            auto program = par.parseProgram();

            // Evaluate (keep program alive for AST references)
            interpreter->execute(*program);
            program_store.push_back(std::move(program));

        } catch (const std::exception& e) {
            // If expression wrapping failed, try as plain statement
            try {
                std::string wrapped = "main {\n" + code + "\n}\n";
                lexer::Lexer lex2(wrapped);
                auto tokens = lex2.tokenize();
                parser::Parser par2(tokens);
                auto program = par2.parseProgram();
                interpreter->execute(*program);
                program_store.push_back(std::move(program));
            } catch (const std::exception& e2) {
                std::cerr << "Error: " << e2.what() << "\n";
            }
        }

        std::cout << ">>> ";
        std::cout.flush();
    }

    std::cout << "\nBye!\n";
    return 0;
}

} // namespace repl
} // namespace naab
