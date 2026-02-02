// NAAb CLI - Main entry point
// Commands: run, parse, check, fmt, blocks, etc.

#include "naab/config.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/interpreter.h"
#include "naab/type_checker.h"
#include "naab/error_reporter.h"
#include "../formatter/formatter.h"
#include "naab/language_registry.h"
#include "naab/block_search_index.h"
#include "naab/block_loader.h"
#include "naab/composition_validator.h"
#include "naab/cpp_executor_adapter.h"
#include "naab/js_executor_adapter.h"
#include "naab/python_executor_adapter.h"
#include "naab/rust_executor.h"
#include "naab/csharp_executor.h"
#include "naab/shell_executor.h"
#include "naab/generic_subprocess_executor.h"
#include "naab/rest_api.h"
#include <fmt/core.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>

// Phase 7c: Initialize language registry with available executors
void initialize_executors() {
    auto& registry = naab::runtime::LanguageRegistry::instance();

    // Register C++ executor
    registry.registerExecutor("cpp",
        std::make_unique<naab::runtime::CppExecutorAdapter>());

    // Register JavaScript executor
    registry.registerExecutor("javascript",
        std::make_unique<naab::runtime::JsExecutorAdapter>());

    // Register Python executor
    #ifdef HAVE_PYBIND11
    fmt::print("[INIT] HAVE_PYBIND11 is defined, registering Python executor\n");
    registry.registerExecutor("python",
        std::make_unique<naab::runtime::PyExecutorAdapter>());
    #else
    fmt::print("[INIT] HAVE_PYBIND11 is NOT defined, Python executor disabled\n");
    #endif

    // Register Rust executor (Phase 3.1-3.3)
    #ifdef HAVE_RUST
    fmt::print("[INIT] HAVE_RUST is defined, registering Rust executor\n");
    registry.registerExecutor("rust",
        std::make_unique<naab::runtime::RustExecutor>());
    #else
    fmt::print("[INIT] HAVE_RUST is NOT defined, Rust executor disabled\n");
    #endif

    // Polyglot Phase 7: Register shell executor
    fmt::print("[INIT] Registering Shell executor\n");
    registry.registerExecutor("shell",
        std::make_unique<naab::runtime::ShellExecutor>());
    registry.registerExecutor("sh",
        std::make_unique<naab::runtime::ShellExecutor>());
    registry.registerExecutor("bash",
        std::make_unique<naab::runtime::ShellExecutor>());

    // Polyglot Phase 7: Register Ruby executor (via GenericSubprocessExecutor)
    fmt::print("[INIT] Registering Ruby executor\n");
    registry.registerExecutor("ruby",
        std::make_unique<naab::runtime::GenericSubprocessExecutor>("ruby", "ruby {}", ".rb"));

    // Polyglot Phase 7: Register Go executor (via GenericSubprocessExecutor)
    fmt::print("[INIT] Registering Go executor\n");
    registry.registerExecutor("go",
        std::make_unique<naab::runtime::GenericSubprocessExecutor>("go", "go run {}", ".go"));

    // Polyglot Phase 11: Register C# executor
    fmt::print("[INIT] Registering C# executor\n");
    registry.registerExecutor("csharp", std::make_unique<naab::runtime::CSharpExecutor>());
    registry.registerExecutor("cs", std::make_unique<naab::runtime::CSharpExecutor>());
}

std::string read_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filename);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void print_usage() {
    fmt::print("NAAb Block Assembly Language v{}\n\n", NAAB_VERSION_STRING);
    fmt::print("Usage:\n");
    fmt::print("  naab-lang run <file.naab>           Execute program\n");
    fmt::print("  naab-lang parse <file.naab>         Show AST\n");
    fmt::print("  naab-lang check <file.naab>         Type check\n");
    fmt::print("  naab-lang fmt <file.naab>           Format code\n");
    fmt::print("  naab-lang validate <block1,block2>  Validate block composition\n");
    fmt::print("  naab-lang stats                     Show usage statistics\n");
    fmt::print("  naab-lang blocks list               List block statistics\n");
    fmt::print("  naab-lang blocks search <query>     Search blocks\n");
    fmt::print("  naab-lang blocks info <block-id>    Show block details\n");
    fmt::print("  naab-lang blocks index [path]       Build search index\n");
    fmt::print("  naab-lang api [port]                Start REST API server\n");
    fmt::print("  naab-lang version                   Show version\n");
    fmt::print("  naab-lang help                      Show this help\n");
    fmt::print("\n");
    fmt::print("Options:\n");
    fmt::print("  --verbose, -v                       Enable verbose output\n");
    fmt::print("  --profile, -p                       Enable performance profiling\n");
    fmt::print("  --explain                           Explain execution step-by-step\n");
    fmt::print("  --debug, -d                         Enable interactive debugger\n");
    fmt::print("  --no-color                          Disable colored error messages\n");
}

int main(int argc, char** argv) {
    // Phase 7c: Initialize language executors
    initialize_executors();

    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];

    if (command == "run") {
        if (argc < 3) {
            fmt::print("Error: Missing file argument\n");
            return 1;
        }
        std::string filename = argv[2];

        // Parse flags and collect script arguments (ISS-028)
        bool verbose = false;
        bool profile = false;
        bool explain = false;
        bool no_color = false;
        bool debug = false;
        std::vector<std::string> script_args;
        for (int i = 3; i < argc; ++i) {
            std::string arg(argv[i]);
            if (arg == "--verbose" || arg == "-v") {
                verbose = true;
            } else if (arg == "--profile" || arg == "-p") {
                profile = true;
            } else if (arg == "--explain") {
                explain = true;
            } else if (arg == "--no-color") {
                no_color = true;
            } else if (arg == "--debug" || arg == "-d") {
                debug = true;
            } else {
                // Non-flag argument - pass to script
                script_args.push_back(arg);
            }
        }

        // Set global color preference for diagnostics (Phase 4.1.32)
        naab::error::Diagnostic::setGlobalColorEnabled(!no_color);

        try {
            // Read source file
            std::string source = read_file(filename);

            // Lex
            naab::lexer::Lexer lexer(source);
            auto tokens = lexer.tokenize();

            // Interpret
            naab::interpreter::Interpreter interpreter;
            interpreter.setVerboseMode(verbose);
            interpreter.setProfileMode(profile);
            interpreter.setExplainMode(explain);
            interpreter.setScriptArgs(script_args);  // ISS-028: Pass script arguments

            // Phase 4.2: Enable interactive debugger
            if (debug) {
                fmt::print("Debug mode enabled. Use 'b <file>:<line>' to set breakpoints.\n");
                // Debugger integration would be enabled here in the interpreter
            }

            // Parse
            interpreter.profileStart("Parsing");
            naab::parser::Parser parser(tokens);
            parser.setSource(source, filename);  // Phase 3.1: Set source for AST location tracking
            auto program = parser.parseProgram();
            interpreter.profileEnd("Parsing");

            // Phase 3.1: Set source code for enhanced error messages
            interpreter.setSourceCode(source, filename);

            interpreter.execute(*program);

            if (profile) {
                interpreter.printProfile();
            }

            return 0;

        } catch (const naab::interpreter::NaabError& e) {
            // NaabError has full stack trace - print it
            fmt::print("{}\n", e.formatError());
            return 1;
        } catch (const std::exception& e) {
            fmt::print("Error: {}\n", e.what());
            return 1;
        }

    } else if (command == "parse") {
        if (argc < 3) {
            fmt::print("Error: Missing file argument\n");
            return 1;
        }
        std::string filename = argv[2];

        try {
            std::string source = read_file(filename);
            naab::lexer::Lexer lexer(source);
            auto tokens = lexer.tokenize();

            naab::parser::Parser parser(tokens);
            parser.setSource(source, filename);  // Phase 3.1: Set source for AST location tracking
            auto program = parser.parseProgram();

            fmt::print("Parsed successfully!\n");
            fmt::print("  Imports: {}\n", program->getImports().size());
            fmt::print("  Functions: {}\n", program->getFunctions().size());
            fmt::print("  Has main: {}\n", program->getMainBlock() ? "yes" : "no");

        } catch (const std::exception& e) {
            fmt::print("Parse error: {}\n", e.what());
            return 1;
        }

    } else if (command == "check") {
        if (argc < 3) {
            fmt::print("Error: Missing file argument\n");
            return 1;
        }
        std::string filename = argv[2];

        try {
            // Read and parse
            std::string source = read_file(filename);
            naab::lexer::Lexer lexer(source);
            auto tokens = lexer.tokenize();

            naab::parser::Parser parser(tokens);
            parser.setSource(source, filename);  // Phase 3.1: Set source for AST location tracking
            auto program = parser.parseProgram();

            // Type check
            naab::typecheck::TypeChecker type_checker;
            auto errors = type_checker.check(std::shared_ptr<naab::ast::Program>(std::move(program)));

            if (errors.empty()) {
                fmt::print("✓ Type check passed: {}\n", filename);
                fmt::print("  No type errors found\n");
                return 0;
            } else {
                fmt::print("✗ Type check failed: {}\n", filename);
                fmt::print("  Found {} type error(s):\n\n", errors.size());
                for (const auto& error : errors) {
                    fmt::print("  {}\n", error.toString());
                }
                return 1;
            }

        } catch (const std::exception& e) {
            fmt::print("Error: {}\n", e.what());
            return 1;
        }

    } else if (command == "fmt") {
        // Phase 4.2: Auto-formatter command
        if (argc < 3) {
            fmt::print("Error: Missing file argument\n");
            fmt::print("Usage: naab-lang fmt [--check] [--config=path] <file.naab>\n");
            return 1;
        }

        // Parse flags
        bool check_only = false;
        bool show_diff = false;
        std::string config_file;
        std::string filename;

        for (int i = 2; i < argc; ++i) {
            std::string arg(argv[i]);
            if (arg == "--check") {
                check_only = true;
            } else if (arg == "--diff") {
                show_diff = true;
            } else if (arg.substr(0, 9) == "--config=") {
                config_file = arg.substr(9);
            } else {
                filename = arg;
            }
        }

        if (filename.empty()) {
            fmt::print("Error: No file specified\n");
            return 1;
        }

        try {
            // Read source file
            std::string source = read_file(filename);

            // Load formatter options
            naab::formatter::FormatterOptions options;
            if (!config_file.empty()) {
                options = naab::formatter::FormatterOptions::fromFile(config_file);
            } else {
                // Try to load .naabfmt.toml from current directory or parent
                std::ifstream config_check(".naabfmt.toml");
                if (config_check.good()) {
                    options = naab::formatter::FormatterOptions::fromFile(".naabfmt.toml");
                } else {
                    options = naab::formatter::FormatterOptions::defaults();
                }
            }

            // Create formatter
            naab::formatter::Formatter formatter(options);

            // Format the source
            std::string formatted = formatter.format(source, filename);

            if (formatter.hasError()) {
                fmt::print("Error: {}\n", formatter.getLastError());
                return 1;
            }

            if (check_only) {
                // Check mode: verify if file is already formatted
                if (source == formatted) {
                    fmt::print("✓ {} is already formatted\n", filename);
                    return 0;
                } else {
                    fmt::print("✗ {} needs formatting\n", filename);
                    if (show_diff) {
                        fmt::print("\nFormatted output:\n{}\n", formatted);
                    }
                    return 1;
                }
            } else {
                // Format in-place
                std::ofstream out_file(filename);
                if (!out_file.is_open()) {
                    fmt::print("Error: Cannot write to file: {}\n", filename);
                    return 1;
                }
                out_file << formatted;
                out_file.close();

                fmt::print("✓ Formatted: {}\n", filename);
                return 0;
            }

        } catch (const std::exception& e) {
            fmt::print("Error: {}\n", e.what());
            return 1;
        }

    } else if (command == "validate") {
        if (argc < 3) {
            fmt::print("Error: Missing block composition argument\n");
            fmt::print("Usage: naab-lang validate <block1,block2,block3>\n");
            fmt::print("Example: naab-lang validate BLOCK-PY-00123,BLOCK-JS-00456\n");
            return 1;
        }

        std::string composition = argv[2];

        try {
            // Parse comma-separated block IDs
            std::vector<std::string> block_ids;
            size_t start = 0;
            size_t end = composition.find(',');

            while (end != std::string::npos) {
                block_ids.push_back(composition.substr(start, end - start));
                start = end + 1;
                end = composition.find(',', start);
            }
            block_ids.push_back(composition.substr(start));

            if (block_ids.size() < 2) {
                fmt::print("Error: Need at least 2 blocks to validate composition\n");
                fmt::print("Example: naab-lang validate BLOCK-PY-00123,BLOCK-JS-00456\n");
                return 1;
            }

            // Initialize block loader
            std::string db_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.naab/blocks.db";
            auto loader = std::make_shared<naab::runtime::BlockLoader>(db_path);

            // Create validator
            naab::validator::CompositionValidator validator(loader);

            fmt::print("Validating block composition...\n");
            fmt::print("  Blocks: ");
            for (size_t i = 0; i < block_ids.size(); i++) {
                if (i > 0) fmt::print(" -> ");
                fmt::print("{}", block_ids[i]);
            }
            fmt::print("\n\n");

            // Validate composition
            auto validation = validator.validate(block_ids);

            if (validation.is_valid) {
                fmt::print("✓ Composition is valid!\n\n");

                // Show type flow
                fmt::print("Type flow:\n");
                for (size_t i = 0; i < validation.type_flow.size(); i++) {
                    fmt::print("  Step {}: {}\n", i, validation.type_flow[i].toString());
                }

                return 0;
            } else {
                fmt::print("✗ Composition has {} type error(s):\n\n", validation.errors.size());

                // Show errors
                for (const auto& error : validation.errors) {
                    fmt::print("Error at position {}:\n", error.position);
                    fmt::print("  {}\n", error.message);

                    if (!error.suggested_adapters.empty()) {
                        fmt::print("  Suggested adapters:\n");
                        for (const auto& adapter : error.suggested_adapters) {
                            fmt::print("    - {}\n", adapter);
                        }
                    }
                    fmt::print("\n");
                }

                // Show suggested fix
                auto suggested_fix = validation.getSuggestedFix();
                if (suggested_fix) {
                    fmt::print("Suggested fix:\n");
                    fmt::print("  {}\n", *suggested_fix);
                }

                return 1;
            }

        } catch (const std::exception& e) {
            fmt::print("Error validating composition: {}\n", e.what());
            fmt::print("Hint: Run 'naab-lang blocks index' to build the block registry\n");
            return 1;
        }

    } else if (command == "stats") {
        try {
            // Initialize block loader
            std::string db_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.naab/blocks.db";
            auto loader = std::make_shared<naab::runtime::BlockLoader>(db_path);

            fmt::print("NAAb Block Usage Statistics\n");
            fmt::print("===========================\n\n");

            // Total blocks
            int total_blocks = loader->getTotalBlocks();
            fmt::print("Total blocks in registry: {}\n\n", total_blocks);

            // Language breakdown
            auto lang_stats = loader->getLanguageStats();
            if (!lang_stats.empty()) {
                fmt::print("Blocks by language:\n");
                for (const auto& [lang, count] : lang_stats) {
                    double percentage = (total_blocks > 0) ? (100.0 * count / total_blocks) : 0.0;
                    fmt::print("  {:12s}: {:6d} blocks ({:5.1f}%)\n", lang, count, percentage);
                }
                fmt::print("\n");
            }

            // Total tokens saved
            long long total_tokens = loader->getTotalTokensSaved();
            fmt::print("Total tokens saved: {:L}\n\n", total_tokens);

            // Top blocks by usage
            auto top_blocks = loader->getTopBlocksByUsage(10);
            if (!top_blocks.empty()) {
                fmt::print("Top 10 most used blocks:\n");
                fmt::print("  Rank  Block ID                    Language      Times Used\n");
                fmt::print("  ----  --------------------------  ------------  ----------\n");
                for (size_t i = 0; i < top_blocks.size(); i++) {
                    const auto& block = top_blocks[i];
                    fmt::print("  {:4d}  {:26s}  {:12s}  {:10d}\n",
                              i + 1,
                              block.block_id.substr(0, 26),
                              block.language.substr(0, 12),
                              block.times_used);
                }
            } else {
                fmt::print("No usage data available yet.\n");
                fmt::print("Blocks will appear here after they are used in programs.\n");
            }

            // Phase 4.4: Show top block combinations
            auto top_combos = loader->getTopCombinations(10);
            if (!top_combos.empty()) {
                fmt::print("\nTop 10 block combinations:\n");
                fmt::print("  Rank  Block 1                     Block 2\n");
                fmt::print("  ----  --------------------------  --------------------------\n");
                for (size_t i = 0; i < top_combos.size(); i++) {
                    const auto& [block1, block2] = top_combos[i];
                    fmt::print("  {:4d}  {:26s}  {:26s}\n",
                              i + 1,
                              block1.substr(0, 26),
                              block2.substr(0, 26));
                }
            }

            return 0;

        } catch (const std::exception& e) {
            fmt::print("Error loading statistics: {}\n", e.what());
            fmt::print("Hint: Run 'naab-lang blocks index' to build the block registry\n");
            return 1;
        }

    } else if (command == "blocks") {
        if (argc < 3) {
            fmt::print("Error: Missing blocks subcommand\n");
            fmt::print("Usage:\n");
            fmt::print("  naab-lang blocks list\n");
            fmt::print("  naab-lang blocks search <query>\n");
            fmt::print("  naab-lang blocks info <block-id>\n");
            fmt::print("  naab-lang blocks index [path]\n");
            return 1;
        }
        std::string subcmd = argv[2];

        if (subcmd == "list") {
            try {
                // Use default blocks database location
                std::string db_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.naab/blocks.db";
                naab::runtime::BlockSearchIndex search_index(db_path);

                int total_blocks = search_index.getBlockCount();
                auto stats = search_index.getStatistics();

                fmt::print("NAAb Block Registry Statistics\n");
                fmt::print("==============================\n\n");
                fmt::print("Total blocks indexed: {}\n", total_blocks);

                if (!stats.empty()) {
                    fmt::print("\nBreakdown by language:\n");
                    for (const auto& [lang, count] : stats) {
                        fmt::print("  {}: {} blocks\n", lang, count);
                    }
                }

                fmt::print("\nUse 'naab-lang blocks search <query>' to search blocks\n");
                return 0;

            } catch (const std::exception& e) {
                fmt::print("Error accessing block registry: {}\n", e.what());
                fmt::print("Hint: Run 'naab-lang blocks index' to build the search index\n");
                return 1;
            }

        } else if (subcmd == "search") {
            if (argc < 4) {
                fmt::print("Error: Missing search query\n");
                fmt::print("Usage: naab-lang blocks search <query>\n");
                return 1;
            }

            std::string query_str = argv[3];

            try {
                // Use default blocks database location
                std::string db_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.naab/blocks.db";
                naab::runtime::BlockSearchIndex search_index(db_path);

                // Build search query
                naab::runtime::SearchQuery query;
                query.query = query_str;
                query.limit = 10;  // Show top 10 results

                // Execute search
                auto results = search_index.search(query);

                if (results.empty()) {
                    fmt::print("No blocks found matching '{}'\n", query_str);
                    return 0;
                }

                fmt::print("Search results for '{}' ({} found)\n", query_str, results.size());
                fmt::print("=================================================\n\n");

                for (size_t i = 0; i < results.size(); i++) {
                    const auto& result = results[i];
                    const auto& meta = result.metadata;

                    fmt::print("{}. {} (score: {:.2f})\n",
                              i + 1, meta.block_id, result.final_score);
                    fmt::print("   Language: {}\n", meta.language);
                    fmt::print("   Description: {}\n", meta.description);

                    if (!meta.input_types.empty() || !meta.output_type.empty()) {
                        fmt::print("   Types: {} -> {}\n",
                                  meta.input_types.empty() ? "void" : meta.input_types,
                                  meta.output_type.empty() ? "void" : meta.output_type);
                    }

                    fmt::print("\n");
                }

                return 0;

            } catch (const std::exception& e) {
                fmt::print("Error searching blocks: {}\n", e.what());
                fmt::print("Hint: Run 'naab-lang blocks index' to build the search index\n");
                return 1;
            }

        } else if (subcmd == "index") {
            // Build search index from blocks directory
            std::string blocks_path;
            if (argc >= 4) {
                blocks_path = argv[3];
            } else {
                // Default to ~/.naab/blocks/library
                blocks_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.naab/blocks/library";
            }

            try {
                std::string db_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.naab/blocks.db";

                fmt::print("Building search index...\n");
                fmt::print("  Source: {}\n", blocks_path);
                fmt::print("  Database: {}\n\n", db_path);

                naab::runtime::BlockSearchIndex search_index(db_path);
                int count = search_index.buildIndex(blocks_path);

                fmt::print("✓ Indexed {} blocks successfully!\n", count);
                fmt::print("\nYou can now use:\n");
                fmt::print("  naab-lang blocks list\n");
                fmt::print("  naab-lang blocks search <query>\n");

                return 0;

            } catch (const std::exception& e) {
                fmt::print("Error building index: {}\n", e.what());
                return 1;
            }

        } else if (subcmd == "info") {
            // ISS-013 Fix: Implement 'blocks info' command
            if (argc < 4) {
                fmt::print("Error: Missing block ID\n");
                fmt::print("Usage: naab-lang blocks info <block-id>\n");
                return 1;
            }

            std::string block_id = argv[3];

            try {
                // Use default blocks database location
                std::string db_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.naab/blocks.db";
                naab::runtime::BlockSearchIndex search_index(db_path);

                // Get block metadata
                auto block_opt = search_index.getBlock(block_id);

                if (!block_opt.has_value()) {
                    fmt::print("Block not found: {}\n", block_id);
                    fmt::print("Use 'naab-lang blocks search <query>' to find blocks\n");
                    return 1;
                }

                const auto& block = block_opt.value();

                // Display block information
                fmt::print("Block Information\n");
                fmt::print("=================\n\n");
                fmt::print("ID:           {}\n", block.block_id);
                fmt::print("Language:     {}\n", block.language);
                fmt::print("Description:  {}\n", block.description);

                if (!block.input_types.empty() || !block.output_type.empty()) {
                    fmt::print("Input Types:  {}\n", block.input_types.empty() ? "void" : block.input_types);
                    fmt::print("Output Type:  {}\n", block.output_type.empty() ? "void" : block.output_type);
                }

                if (!block.category.empty()) {
                    fmt::print("Category:     {}\n", block.category);
                }

                if (!block.tags.empty()) {
                    fmt::print("Tags:         ");
                    for (size_t i = 0; i < block.tags.size(); i++) {
                        if (i > 0) fmt::print(", ");
                        fmt::print("{}", block.tags[i]);
                    }
                    fmt::print("\n");
                }

                if (!block.performance_tier.empty()) {
                    fmt::print("Performance:  {}\n", block.performance_tier);
                }

                if (block.success_rate_percent >= 0) {
                    fmt::print("Success Rate: {}%\n", block.success_rate_percent);
                }

                if (block.avg_tokens_saved > 0) {
                    fmt::print("Tokens Saved: {}\n", block.avg_tokens_saved);
                }

                if (block.times_used > 0) {
                    fmt::print("Times Used:   {}\n", block.times_used);
                }

                return 0;

            } catch (const std::exception& e) {
                fmt::print("Error getting block info: {}\n", e.what());
                fmt::print("Hint: Run 'naab-lang blocks index' to build the search index\n");
                return 1;
            }

        } else {
            fmt::print("Unknown blocks subcommand: {}\n", subcmd);
            fmt::print("Available: list, search, index, info\n");
            return 1;
        }

    } else if (command == "api") {
        // Start REST API server (Phase 4.7)
        int port = 8080; // Default port
        if (argc >= 3) {
            try {
                port = std::stoi(argv[2]);
            } catch (...) {
                fmt::print("Error: Invalid port number\n");
                return 1;
            }
        }

        try {
            // Create REST API server
            naab::api::RestApiServer server(port, "0.0.0.0");

            // Set up block loader for API endpoints
            std::string db_path = NAAB_DATABASE_PATH;
            auto loader = std::make_shared<naab::runtime::BlockLoader>(db_path);
            server.setBlockLoader(loader);

            // Create and set up interpreter for code execution
            auto interpreter = std::make_shared<naab::interpreter::Interpreter>();
            server.setInterpreter(interpreter);

            fmt::print("\n");
            fmt::print("╔════════════════════════════════════════════════════╗\n");
            fmt::print("║  NAAb REST API Server v{}                      ║\n", NAAB_VERSION_STRING);
            fmt::print("╚════════════════════════════════════════════════════╝\n");
            fmt::print("\n");

            // Start server (blocking)
            if (!server.start()) {
                fmt::print("Failed to start server\n");
                return 1;
            }

        } catch (const std::exception& e) {
            fmt::print("Error starting API server: {}\n", e.what());
            return 1;
        }

    } else if (command == "version") {
        auto& registry = naab::runtime::LanguageRegistry::instance();
        auto languages = registry.supportedLanguages();

        fmt::print("NAAb Block Assembly Language v{}\n", NAAB_VERSION_STRING);
        fmt::print("Git: {}\n", NAAB_GIT_HASH);
        fmt::print("Built: {}\n", NAAB_BUILD_TIMESTAMP);
        fmt::print("API Version: {}\n", NAAB_API_VERSION);
        fmt::print("Supported languages: ");
        for (size_t i = 0; i < languages.size(); i++) {
            if (i > 0) fmt::print(", ");
            fmt::print("{}", languages[i]);
        }
        fmt::print("\n");

    } else if (command == "help") {
        print_usage();

    } else {
        fmt::print("Unknown command: {}\n", command);
        print_usage();
        return 1;
    }

    return 0;
}
