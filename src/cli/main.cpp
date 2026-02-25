// NAAb CLI - Main entry point
// Commands: run, parse, check, fmt, blocks, etc.

#include "naab/config.h"
#include "naab/paths.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/interpreter.h"
#include "naab/type_checker.h"
#include "naab/error_reporter.h"
#include "../formatter/formatter.h"
#include "naab/language_registry.h"
#include "naab/block_search_index.h"
#include "naab/block_registry.h"
#include "naab/block_loader.h"
#include "naab/composition_validator.h"
#include "naab/cpp_executor_adapter.h"
#include "naab/js_executor_adapter.h"
#include "naab/python_executor_adapter.h"
// #include "naab/python_executor.h"  // REMOVED: Using pure C API (PythonCExecutor) now
#include "naab/python_interpreter_manager.h"
#include "naab/polyglot_async_executor.h"
#include "naab/rust_executor.h"
#include "naab/csharp_executor.h"
#include "naab/go_executor.h"
#include "naab/shell_executor.h"
#include "naab/generic_subprocess_executor.h"
#include "naab/rest_api.h"
#include "naab/manifest.h"
#include "naab/logger.h"
#include "naab/sandbox.h"
#include "naab/resource_limits.h"
#include "naab/stdlib.h"  // For setPipeMode()
#include "naab/governance.h"  // For governance report CLI flags
#include <fmt/core.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <unistd.h>  // _exit()
#include <filesystem>

// Enterprise security configuration for polyglot blocks
naab::security::SandboxConfig createEnterpriseConfig() {
    naab::security::SandboxConfig config;

    // Filesystem: Allow read/write in current directory and /tmp only
    config.addCapability(naab::security::Capability::FS_READ);
    config.addCapability(naab::security::Capability::FS_WRITE);
    config.allow_fork = false;  // Prevent fork bombs
    config.allow_exec = false;  // Prevent arbitrary command execution

    // Block interaction: Allow polyglot blocks to execute
    config.addCapability(naab::security::Capability::BLOCK_CALL);

    // Network: Disabled by default for security
    config.network_enabled = false;

    // Resource limits
    config.max_memory_mb = 2048;    // 2GB per block (Python needs more for allocator)
    config.max_cpu_seconds = 30;    // 30 second timeout
    config.max_file_size_mb = 100;  // 100MB file limit

    // Allowed paths
    std::filesystem::path cwd = std::filesystem::current_path();
    std::filesystem::path tmp = naab::paths::temp_dir();
    config.allowReadPath(cwd.string());
    config.allowWritePath(cwd.string());
    config.allowReadPath(tmp.string());
    config.allowWritePath(tmp.string());

    return config;
}

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
    // Initialize Python interpreter before creating executor
    naab::runtime::PythonInterpreterManager::initialize();
    registry.registerExecutor("python",
        std::make_unique<naab::runtime::PyExecutorAdapter>());

    // Create thread pool for parallel polyglot execution
    naab::polyglot::initializePolyglotThreadPool();
    #endif

    // Register Rust executor (Phase 3.1-3.3)
    #ifdef HAVE_RUST
    registry.registerExecutor("rust",
        std::make_unique<naab::runtime::RustExecutor>());
    #endif

    // Polyglot Phase 7: Register shell executor
    registry.registerExecutor("shell",
        std::make_unique<naab::runtime::ShellExecutor>());
    registry.registerExecutor("sh",
        std::make_unique<naab::runtime::ShellExecutor>());
    registry.registerExecutor("bash",
        std::make_unique<naab::runtime::ShellExecutor>());

    // Polyglot Phase 7: Register Ruby executor (via GenericSubprocessExecutor)
    registry.registerExecutor("ruby",
        std::make_unique<naab::runtime::GenericSubprocessExecutor>("ruby", "ruby {}", ".rb"));

    // Issue #3: Register dedicated Go executor
    registry.registerExecutor("go", std::make_unique<naab::runtime::GoExecutor>());
    registry.registerExecutor("golang", std::make_unique<naab::runtime::GoExecutor>());

    // Polyglot Phase 11: Register C# executor
    registry.registerExecutor("csharp", std::make_unique<naab::runtime::CSharpExecutor>());
    registry.registerExecutor("cs", std::make_unique<naab::runtime::CSharpExecutor>());

    // Register PHP executor (via GenericSubprocessExecutor)
    registry.registerExecutor("php",
        std::make_unique<naab::runtime::GenericSubprocessExecutor>("php", "php {}", ".php"));

    // Register TypeScript executor (via GenericSubprocessExecutor)
    registry.registerExecutor("typescript",
        std::make_unique<naab::runtime::GenericSubprocessExecutor>("typescript", "tsx {}", ".ts"));
    registry.registerExecutor("ts",
        std::make_unique<naab::runtime::GenericSubprocessExecutor>("ts", "tsx {}", ".ts"));
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
    fmt::print("  naab-lang init                      Create naab.toml manifest\n");
    fmt::print("  naab-lang manifest check            Validate naab.toml\n");
    fmt::print("  naab-lang version                   Show version\n");
    fmt::print("  naab-lang help                      Show this help\n");
    fmt::print("\n");
    fmt::print("Options:\n");
    fmt::print("  --verbose, -v                       Enable verbose output\n");
    fmt::print("  --profile, -p                       Enable performance profiling\n");
    fmt::print("  --explain                           Explain execution step-by-step\n");
    fmt::print("  --debug, -d                         Enable interactive debugger\n");
    fmt::print("  --no-color                          Disable colored error messages\n");
    fmt::print("  --pipe                              Pipe mode: io.write() → stderr,\n");
    fmt::print("                                      io.output() → stdout (for JSON)\n");
    fmt::print("\nGovernance Options:\n");
    fmt::print("  --governance-override               Override soft-mandatory governance rules\n");
    fmt::print("  --governance-report <path>          Write JSON governance report to file\n");
    fmt::print("  --governance-sarif <path>           Write SARIF governance report to file\n");
    fmt::print("  --governance-junit <path>           Write JUnit governance report to file\n");
    fmt::print("\nSecurity Options:\n");
    fmt::print("  --sandbox-level <level>             Security: restricted|standard|elevated|unrestricted\n");
    fmt::print("                                      (default: standard - safe for enterprise)\n");
    fmt::print("  --timeout <seconds>                 Execution timeout per block (default: 30)\n");
    fmt::print("  --memory-limit <MB>                 Memory limit per block (default: 512)\n");
    fmt::print("  --allow-network                     Enable network access (default: disabled)\n");
}

int main(int argc, char** argv) {
    // Export interpreter path so polyglot scripts can find naab-lang
    // This solves the common problem of Python/shell scripts not knowing
    // where the NAAb interpreter is located
    {
        std::error_code ec;
        auto exe_path = std::filesystem::canonical(argv[0], ec);
        if (!ec) {
            setenv("NAAB_INTERPRETER_PATH", exe_path.c_str(), 1);
            setenv("NAAB_LANGUAGE_DIR", exe_path.parent_path().parent_path().c_str(), 1);
        } else {
            // Fallback: use argv[0] as-is
            setenv("NAAB_INTERPRETER_PATH", argv[0], 1);
        }
    }

    // Phase 7c: Initialize language executors
    initialize_executors();

    if (argc < 2) {
        print_usage();
        fflush(stdout);
        return 1;
    }

    // Pre-scan for global flags that can appear before the command
    // e.g., `naab-lang --pipe script.naab` or `naab-lang --governance-override script.naab`
    bool global_pipe_mode = false;
    bool global_governance_override = false;
    int command_arg_index = 1;  // Index of the actual command/file in argv

    while (command_arg_index < argc) {
        std::string arg(argv[command_arg_index]);
        if (arg == "--pipe") {
            global_pipe_mode = true;
            command_arg_index++;
        } else if (arg == "--governance-override") {
            global_governance_override = true;
            command_arg_index++;
        } else {
            break;  // Found the command or file
        }
    }

    if (command_arg_index >= argc) {
        print_usage();
        fflush(stdout);
        return 1;
    }

    std::string command = argv[command_arg_index];

    // Handle --help and -h flags (common user expectation)
    if (command == "--help" || command == "-h") {
        print_usage();
        fflush(stdout);
        _exit(0);
    }

    // Handle --version and -V flags
    if (command == "--version" || command == "-V") {
        fmt::print("naab-lang {}\n", NAAB_VERSION_STRING);
        fflush(stdout);
        _exit(0);
    }

    // Auto-detect .naab files: `naab-lang file.naab` → `naab-lang run file.naab`
    // This is the #1 source of confusion for new users and LLMs
    bool auto_run = false;
    if (command.size() > 5 && command.substr(command.size() - 5) == ".naab") {
        auto_run = true;
        command = "run";
    }

    if (command == "run") {
        if (!auto_run && command_arg_index + 1 >= argc) {
            fmt::print("Error: Missing file argument. Usage: naab-lang run <file.naab>\n");
            return 1;
        }

        // Parse flags first, then find filename (ISS-028)
        bool verbose = false;
        bool profile = false;
        bool explain = false;
        bool no_color = false;
        bool debug = false;
        bool pipe_mode = global_pipe_mode;  // Inherit from global pre-scan
        bool governance_override = global_governance_override;
        std::string governance_report_json;
        std::string governance_report_sarif;
        std::string governance_report_junit;
        std::string sandbox_level = "unrestricted";  // Default: full language power
        unsigned int timeout = 30;
        size_t memory_limit = 512;
        bool network_enabled = false;
        std::string filename;
        std::vector<std::string> script_args;

        // When auto-detected, command_arg_index points to the .naab file
        // Otherwise, command_arg_index points to "run", so start at +1
        int arg_start = auto_run ? command_arg_index : command_arg_index + 1;
        for (int i = arg_start; i < argc; ++i) {
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
            } else if (arg == "--pipe") {
                pipe_mode = true;
            } else if (arg == "--sandbox-level" && i + 1 < argc) {
                sandbox_level = argv[++i];
            } else if (arg == "--timeout" && i + 1 < argc) {
                timeout = std::stoi(argv[++i]);
            } else if (arg == "--memory-limit" && i + 1 < argc) {
                memory_limit = std::stoull(argv[++i]);
            } else if (arg == "--allow-network") {
                network_enabled = true;
            } else if (arg == "--governance-override") {
                governance_override = true;
            } else if (arg == "--governance-report" && i + 1 < argc) {
                governance_report_json = argv[++i];
            } else if (arg == "--governance-sarif" && i + 1 < argc) {
                governance_report_sarif = argv[++i];
            } else if (arg == "--governance-junit" && i + 1 < argc) {
                governance_report_junit = argv[++i];
            } else if (arg.substr(0, 2) == "--") {
                // Unknown flag — give helpful error instead of treating as filename
                fmt::print("Error: Unknown flag '{}'\n\n"
                           "  Available flags:\n"
                           "    --verbose, -v         Enable verbose output\n"
                           "    --profile, -p         Enable performance profiling\n"
                           "    --explain             Explain execution step-by-step\n"
                           "    --debug, -d           Enable interactive debugger\n"
                           "    --no-color            Disable colored error messages\n"
                           "    --pipe                Pipe mode (io.write→stderr, io.output→stdout)\n"
                           "    --sandbox-level <L>   Security level\n"
                           "    --timeout <seconds>   Execution timeout per block\n"
                           "    --memory-limit <MB>   Memory limit per block\n"
                           "    --allow-network       Enable network access\n"
                           "    --governance-override Override soft-mandatory governance rules\n"
                           "    --governance-report <path>  Write JSON governance report\n"
                           "    --governance-sarif <path>   Write SARIF governance report\n"
                           "    --governance-junit <path>   Write JUnit governance report\n\n"
                           "  Note: There is no --path flag. NAAb resolves modules relative to\n"
                           "  the script's directory. To use modules from another location,\n"
                           "  place the script in or near the modules directory, or use\n"
                           "  absolute paths in 'use' statements.\n", arg);
                fflush(stdout);
                return 1;
            } else {
                // Non-flag argument
                if (filename.empty()) {
                    // First non-flag is the filename
                    filename = arg;
                } else {
                    // Subsequent non-flags are script arguments
                    script_args.push_back(arg);
                }
            }
        }

        // Validate filename was provided
        if (filename.empty()) {
            fmt::print("Error: Missing file argument. Usage: naab-lang run <file.naab>\n");
            return 1;
        }

        // Configure logger based on verbosity
        naab::logging::Logger::instance().setVerbose(verbose);

        // Set global color preference for diagnostics (Phase 4.1.32)
        naab::error::Diagnostic::setGlobalColorEnabled(!no_color);

        // Initialize security sandbox (enterprise hardening)
        naab::security::SandboxConfig security_config;
        if (sandbox_level == "restricted") {
            security_config = naab::security::SandboxConfig::fromPermissionLevel(
                naab::security::PermissionLevel::RESTRICTED
            );
        } else if (sandbox_level == "standard") {
            security_config = createEnterpriseConfig();  // Safe default
        } else if (sandbox_level == "elevated") {
            security_config = naab::security::SandboxConfig::fromPermissionLevel(
                naab::security::PermissionLevel::ELEVATED
            );
        } else if (sandbox_level == "unrestricted") {
            security_config = naab::security::SandboxConfig::fromPermissionLevel(
                naab::security::PermissionLevel::UNRESTRICTED
            );
        } else {
            fmt::print("Error: Invalid sandbox level '{}'. Use: restricted|standard|elevated|unrestricted\n", sandbox_level);
            return 1;
        }

        // Apply CLI overrides to security config
        security_config.max_cpu_seconds = timeout;
        security_config.max_memory_mb = memory_limit;
        security_config.network_enabled = network_enabled;

        // Set default config for SandboxManager
        naab::security::SandboxManager::instance().setDefaultConfig(security_config);

        // Configure Python import blocking based on sandbox level
        // NOTE: Temporarily disabled while using pure C API (PythonCExecutor)
        // TODO: Re-implement import blocking in PythonCExecutor for security
        // Unrestricted mode allows all imports (including os, subprocess, etc.)
        // if (sandbox_level == "unrestricted") {
        //     naab::runtime::PythonExecutor::setBlockDangerousImports(false);
        // } else {
        //     naab::runtime::PythonExecutor::setBlockDangerousImports(true);
        // }

        if (verbose) {
            fmt::print("[Security] Sandbox level: {}, timeout: {}s, memory: {}MB, network: {}\n",
                       sandbox_level, timeout, memory_limit, network_enabled ? "enabled" : "disabled");
        }

        // Load manifest if available
        auto manifest = naab::manifest::ManifestLoader::findAndLoad(".");
        if (manifest.has_value()) {
            // Manifest loaded - configuration will be applied by interpreter
            if (verbose) {
                fmt::print("[Manifest] Using project: {} v{}\n",
                           manifest->package.name, manifest->package.version);
            }
        } else {
            // No manifest - use defaults
            if (verbose) {
                fmt::print("[Manifest] No naab.toml found, using defaults\n");
            }
        }

        // Activate pipe mode: io.write() → stderr, io.output() → stdout
        // Use --pipe when calling NAAb as a subprocess and parsing stdout as JSON
        if (pipe_mode) {
            naab::stdlib::setPipeMode(true);
        }

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
            if (governance_override) {
                interpreter.setGovernanceOverride(true);
            }

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

            // CLI governance report path overrides (after govern.json is loaded)
            if (!governance_report_json.empty() || !governance_report_sarif.empty() ||
                !governance_report_junit.empty()) {
                auto* gov = interpreter.getGovernance();
                if (gov) {
                    auto& rules = gov->getMutableRules();
                    if (!governance_report_json.empty())
                        rules.output.file_output.report_json = governance_report_json;
                    if (!governance_report_sarif.empty())
                        rules.output.file_output.report_sarif = governance_report_sarif;
                    if (!governance_report_junit.empty())
                        rules.output.file_output.report_junit = governance_report_junit;
                }
            }

            interpreter.execute(*program);

            if (profile) {
                interpreter.printProfile();
            }

            // Use _exit() after interpreter runs - thread pool workers and
            // Python thread states trigger bionic CFI crashes during static
            // destruction on Android. _exit() is safe: the OS cleans up all
            // process resources, and we've already flushed all output.
            fflush(stdout);
            fflush(stderr);
            _exit(0);

        } catch (const naab::interpreter::NaabError& e) {
            // NaabError has full stack trace - print it
            fmt::print("{}\n", e.formatError());
            fflush(stdout);
            fflush(stderr);
            _exit(1);
        } catch (const std::exception& e) {
            fmt::print("Error: {}\n", e.what());
            fflush(stdout);
            fflush(stderr);
            _exit(1);
        }

    } else if (command == "parse") {
        if (argc < 3) {
            fmt::print("Error: Missing file argument. Usage: naab-lang run <file.naab>\n");
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
            fmt::print("Error: Missing file argument. Usage: naab-lang run <file.naab>\n");
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
            fmt::print("Error: Missing file argument. Usage: naab-lang run <file.naab>\n");
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

        // --diff implies --check (show diff without modifying file)
        if (show_diff) {
            check_only = true;
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
            fmt::print("  naab-lang blocks list [--language <lang>] [--category <cat>]\n");
            fmt::print("  naab-lang blocks search <query> [--language <lang>] [--category <cat>]\n");
            fmt::print("  naab-lang blocks info <block-id>\n");
            fmt::print("  naab-lang blocks similar <block-id>\n");
            fmt::print("  naab-lang blocks index [path]\n");
            fmt::print("  naab-lang blocks stats\n");
            fmt::print("  naab-lang blocks export <block-id>\n");
            fmt::print("  naab-lang blocks import <file.json>\n");
            fmt::print("  naab-lang blocks backup / restore\n");
            return 1;
        }
        std::string subcmd = argv[2];

        if (subcmd == "list") {
            // Parse optional --language and --category flags
            std::string language_filter;
            std::string category_filter;
            for (int i = 3; i < argc; i++) {
                std::string arg(argv[i]);
                if (arg == "--language" && i + 1 < argc) {
                    language_filter = argv[++i];
                } else if (arg == "--category" && i + 1 < argc) {
                    category_filter = argv[++i];
                }
            }

            try {
                // Use default blocks database location
                std::string db_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.naab/blocks.db";
                naab::runtime::BlockSearchIndex search_index(db_path);

                if (!language_filter.empty() || !category_filter.empty()) {
                    // Filtered list via search
                    naab::runtime::SearchQuery query;
                    query.query = "*";
                    query.limit = 100;
                    if (!language_filter.empty()) query.language = language_filter;
                    if (!category_filter.empty()) query.category = category_filter;
                    auto results = search_index.search(query);

                    fmt::print("NAAb Blocks");
                    if (!language_filter.empty()) fmt::print(" [language: {}]", language_filter);
                    if (!category_filter.empty()) fmt::print(" [category: {}]", category_filter);
                    fmt::print("\n{}\n\n", std::string(40, '='));

                    for (const auto& result : results) {
                        fmt::print("  {} - {}\n", result.metadata.block_id, result.metadata.description);
                    }
                    fmt::print("\n{} blocks found\n", results.size());
                } else {
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
                    fmt::print("Filter: 'naab-lang blocks list --language <lang>'\n");
                }
                return 0;

            } catch (const std::exception& e) {
                fmt::print("Error accessing block registry: {}\n", e.what());
                fmt::print("Hint: Run 'naab-lang blocks index' to build the search index\n");
                return 1;
            }

        } else if (subcmd == "search") {
            if (argc < 4) {
                fmt::print("Error: Missing search query\n");
                fmt::print("Usage: naab-lang blocks search <query> [--language <lang>] [--category <cat>]\n");
                return 1;
            }

            // Parse search arguments with optional --language and --category flags
            std::string query_str;
            std::string language_filter;
            std::string category_filter;
            for (int i = 3; i < argc; i++) {
                std::string arg(argv[i]);
                if (arg == "--language" && i + 1 < argc) {
                    language_filter = argv[++i];
                } else if (arg == "--category" && i + 1 < argc) {
                    category_filter = argv[++i];
                } else if (query_str.empty()) {
                    query_str = arg;
                }
            }

            if (query_str.empty()) {
                fmt::print("Error: Missing search query\n");
                return 1;
            }

            try {
                // Use default blocks database location
                std::string db_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.naab/blocks.db";
                naab::runtime::BlockSearchIndex search_index(db_path);

                // Build search query
                naab::runtime::SearchQuery query;
                query.query = query_str;
                query.limit = 10;  // Show top 10 results
                if (!language_filter.empty()) {
                    query.language = language_filter;
                }
                if (!category_filter.empty()) {
                    query.category = category_filter;
                }

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
                // Default to ~/.naab/language/blocks/library
                blocks_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.naab/language/blocks/library";
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

        } else if (subcmd == "similar") {
            if (argc < 4) {
                fmt::print("Error: Missing block ID\n");
                fmt::print("Usage: naab-lang blocks similar <block-id>\n");
                return 1;
            }
            std::string block_id = argv[3];
            try {
                std::string db_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.naab/blocks.db";
                naab::runtime::BlockSearchIndex search_index(db_path);

                // Get block metadata to use as search query
                auto block_opt = search_index.getBlock(block_id);
                if (!block_opt.has_value()) {
                    fmt::print("Block not found: {}\n", block_id);
                    return 1;
                }

                // Search using the block's description and keywords
                naab::runtime::SearchQuery query;
                query.query = block_opt->description;
                if (!block_opt->language.empty()) {
                    query.language = block_opt->language;
                }
                query.limit = 11;  // Extra one to skip self

                auto results = search_index.search(query);

                fmt::print("Blocks similar to {}\n", block_id);
                fmt::print("{}\n\n", std::string(40, '='));

                int shown = 0;
                for (const auto& result : results) {
                    if (result.metadata.block_id == block_id) continue;  // Skip self
                    fmt::print("  {} (score: {:.2f})\n", result.metadata.block_id, result.final_score);
                    fmt::print("    {}\n\n", result.metadata.description);
                    if (++shown >= 10) break;
                }

                if (shown == 0) {
                    fmt::print("  No similar blocks found\n");
                }
                return 0;
            } catch (const std::exception& e) {
                fmt::print("Error: {}\n", e.what());
                fmt::print("Hint: Run 'naab-lang blocks index' first\n");
                return 1;
            }

        } else if (subcmd == "stats") {
            // Alias to top-level stats command
            try {
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
                return 0;
            } catch (const std::exception& e) {
                fmt::print("Error: {}\n", e.what());
                fmt::print("Hint: Run 'naab-lang blocks index' first\n");
                return 1;
            }

        } else if (subcmd == "export") {
            if (argc < 4) {
                fmt::print("Error: Missing block ID\n");
                fmt::print("Usage: naab-lang blocks export <block-id>\n");
                return 1;
            }
            std::string block_id = argv[3];

            // Use BlockRegistry to find the block's JSON file
            std::string blocks_dir = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.naab/language/blocks/library";
            naab::runtime::BlockRegistry::instance().initialize(blocks_dir);
            auto block_opt = naab::runtime::BlockRegistry::instance().getBlock(block_id);

            if (!block_opt.has_value()) {
                fmt::print("Block not found: {}\n", block_id);
                return 1;
            }

            // Read and print the raw JSON file
            std::ifstream file(block_opt->file_path);
            if (!file.is_open()) {
                fmt::print("Error: Cannot read block file: {}\n", block_opt->file_path);
                return 1;
            }
            std::string content((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
            fmt::print("{}\n", content);
            return 0;

        } else if (subcmd == "import") {
            if (argc < 4) {
                fmt::print("Error: Missing file path\n");
                fmt::print("Usage: naab-lang blocks import <file.json>\n");
                return 1;
            }
            std::string file_path = argv[3];

            // Read and parse the JSON file
            std::ifstream file(file_path);
            if (!file.is_open()) {
                fmt::print("Error: Cannot open file: {}\n", file_path);
                return 1;
            }
            std::string content((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());

            // Extract "id" and "language" fields with simple string search
            auto extractField = [&](const std::string& field) -> std::string {
                std::string key = "\"" + field + "\"";
                auto pos = content.find(key);
                if (pos == std::string::npos) return "";
                pos = content.find("\"", pos + key.size() + 1);  // skip past colon
                if (pos == std::string::npos) return "";
                auto end = content.find("\"", pos + 1);
                if (end == std::string::npos) return "";
                return content.substr(pos + 1, end - pos - 1);
            };

            std::string block_id = extractField("id");
            std::string language = extractField("language");

            if (language.empty() || block_id.empty()) {
                fmt::print("Error: JSON must contain 'id' and 'language' fields\n");
                return 1;
            }

            // Copy to appropriate language directory
            std::string dest_dir = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") +
                "/.naab/language/blocks/library/" + language;
            std::string dest_path = dest_dir + "/" + block_id + ".json";

            std::ofstream out(dest_path);
            if (!out.is_open()) {
                fmt::print("Error: Cannot write to {}\n", dest_path);
                return 1;
            }
            out << content;
            out.close();

            fmt::print("Imported {} to {}\n", block_id, dest_path);
            fmt::print("Run 'naab-lang blocks index' to update the search index\n");
            return 0;

        } else if (subcmd == "create" || subcmd == "test" || subcmd == "submit") {
            fmt::print("'blocks {}' is not yet implemented.\n\n", subcmd);
            if (subcmd == "create") {
                fmt::print("To create a block manually:\n");
                fmt::print("  1. Create a JSON file with id, language, code, and description fields\n");
                fmt::print("  2. Place it in ~/.naab/language/blocks/library/<language>/\n");
                fmt::print("  3. Run 'naab-lang blocks index' to update the registry\n");
            } else if (subcmd == "test") {
                fmt::print("To test a block, create a .naab file:\n");
                fmt::print("  use BLOCK-ID as alias\n");
                fmt::print("  main { let result = alias.function_name(args) }\n");
            } else {
                fmt::print("Block submission to public registry is planned for a future release.\n");
            }
            return 1;

        } else if (subcmd == "update") {
            fmt::print("Blocks are currently local-only.\n");
            fmt::print("Use 'naab-lang blocks index' to rebuild the search index from local files.\n");
            return 0;

        } else if (subcmd == "backup") {
            std::string db_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.naab/blocks.db";
            std::string backup_path = db_path + ".backup";
            std::ifstream src(db_path, std::ios::binary);
            if (!src.is_open()) {
                fmt::print("Error: Cannot open blocks.db for backup\n");
                return 1;
            }
            std::ofstream dst(backup_path, std::ios::binary);
            dst << src.rdbuf();
            fmt::print("Backed up blocks.db to {}\n", backup_path);
            return 0;

        } else if (subcmd == "restore") {
            std::string db_path = std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.naab/blocks.db";
            std::string backup_path = db_path + ".backup";
            std::ifstream src(backup_path, std::ios::binary);
            if (!src.is_open()) {
                fmt::print("Error: No backup found at {}\n", backup_path);
                return 1;
            }
            std::ofstream dst(db_path, std::ios::binary);
            dst << src.rdbuf();
            fmt::print("Restored blocks.db from backup\n");
            return 0;

        } else if (subcmd == "report") {
            if (argc < 4) {
                fmt::print("Error: Missing block ID\n");
                fmt::print("Usage: naab-lang blocks report <block-id>\n");
                return 1;
            }
            fmt::print("Block issue reporting is planned for a future release.\n");
            fmt::print("For now, please file issues at the NAAb project repository.\n");
            return 0;

        } else {
            fmt::print("Unknown blocks subcommand: {}\n", subcmd);
            fmt::print("Available: list, search, index, info, similar, stats, export, import,\n");
            fmt::print("           create, test, submit, update, backup, restore, report\n");
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

    } else if (command == "init") {
        // Create default naab.toml
        if (naab::manifest::createDefaultManifest("naab.toml")) {
            fmt::print("✓ Created naab.toml\n");
            return 0;
        } else {
            fmt::print("✗ Failed to create naab.toml\n");
            return 1;
        }

    } else if (command == "manifest") {
        // Handle manifest subcommands
        if (argc < 3) {
            fmt::print("Error: Missing manifest subcommand\n");
            fmt::print("Usage: naab-lang manifest check\n");
            return 1;
        }

        std::string subcommand = argv[2];
        if (subcommand == "check") {
            auto manifest = naab::manifest::ManifestLoader::load("naab.toml");
            if (manifest.has_value()) {
                fmt::print("✓ naab.toml is valid\n");
                fmt::print("  Package: {} v{}\n", manifest->package.name, manifest->package.version);
                if (!manifest->package.description.empty()) {
                    fmt::print("  Description: {}\n", manifest->package.description);
                }
                fmt::print("  Build target: {}\n", manifest->build.target);
                fmt::print("  Optimize: {}\n", manifest->build.optimize ? "true" : "false");
                return 0;
            } else {
                fmt::print("✗ Error: {}\n", naab::manifest::ManifestLoader::getLastError());
                return 1;
            }
        } else {
            fmt::print("Unknown manifest subcommand: {}\n", subcommand);
            return 1;
        }

    } else if (command == "help") {
        print_usage();

    } else {
        fmt::print("Unknown command: {}\n\n", command);
        print_usage();
        fflush(stdout);
        fflush(stderr);
        _exit(1);
    }

    // Use _exit() to skip static destructors on Android.
    // Thread pool workers and Python thread states trigger bionic CFI
    // crashes during static destruction (mmap fails for shadow memory
    // late in process lifetime). _exit() is safe: the OS cleans up all
    // process resources, and we've already flushed all output.
    _exit(0);
}
