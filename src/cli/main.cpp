// NAAb CLI - Main entry point
// Commands: run, parse, check, fmt, blocks, etc.

#ifdef _WIN32
#  include <io.h>
#  define isatty _isatty
#  define fileno _fileno
#  define popen _popen
#  define pclose _pclose
#endif

#include "repl.h"
#include "naab/config.h"
#include "naab/paths.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/interpreter.h"
#include "naab/vm.h"
#include "naab/compiler.h"
#include "naab/module_resolver.h"
#include "naab/type_checker.h"
#include "naab/error_reporter.h"
#include "naab/error_sanitizer.h"
#include "../formatter/formatter.h"
#include "naab/language_registry.h"
#include "naab/block_search_index.h"
#include "naab/block_registry.h"
#include "naab/block_loader.h"
#include "naab/composition_validator.h"
// Cross-platform executor headers
#ifdef HAVE_QUICKJS
#include "naab/js_executor_adapter.h"
#endif
#include "naab/generic_subprocess_executor.h"

// POSIX-only executor headers
#ifndef _WIN32
#include "naab/cpp_executor_adapter.h"
#ifdef HAVE_PYBIND11
#include "naab/python_executor_adapter.h"
#include "naab/python_interpreter_manager.h"
#endif
#include "naab/polyglot_async_executor.h"
#include "naab/rust_executor.h"
#include "naab/csharp_executor.h"
#include "naab/go_executor.h"
#include "naab/nim_executor.h"
#include "naab/zig_executor.h"
#include "naab/julia_executor.h"
#include "naab/shell_executor.h"
#include "naab/persistent_shell_executor.h"
#include "naab/node_persistent_executor.h"
#include "naab/persistent_ruby_executor.h"
#endif // !_WIN32
#include "naab/rest_api.h"
#include "naab/manifest.h"
#include "naab/logger.h"
#include "naab/sandbox.h"
#include "naab/resource_limits.h"
#include "naab/limits.h"
#include "naab/stdlib.h"  // For setPipeMode()
#include "naab/stdlib_new_modules.h"  // For EnvModule::setArgsProvider
#include "naab/governance.h"  // For governance report CLI flags
#include "naab/crypto_utils.h"   // V-SC-009: Ed25519 keygen
#include "naab/trust_store.h"    // V-SC-009: Trust store management
#include "naab/secure_file.h"    // V-SC-009: Secure file writes
#include "naab/scanner.h"    // For --scan command
#include "naab/lockfile.h"   // For --lock / --lock-check flags (Phase 8.4)
#include "governance_init.h"  // For naab-lang init --governance
#include "naab/package_manager.h"  // For package management commands
#include "naab/profiler.h"   // For VM profiler integration
#include "naab/debugger.h"   // For VM debugger integration
#include "naab/platform.h"
#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <filesystem>
#ifndef _WIN32
#  include <unistd.h>  // _exit()
#else
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
   // windows.h defines OUT as a SAL annotation macro (expands to empty).
   // This collides with naab::debugger::StepMode::OUT.
#  undef OUT
#endif
#include <atomic>        // Ctrl-C handler counter (Windows)
#include <chrono>        // Calibration timing
#include <map>           // Calibration data

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

// Fail-closed: Sync governance capabilities into sandbox (tighten only, never loosen)
// Called after governance loads and after mid-run reload to ensure both layers agree.
static void syncGovernanceToSandbox(
    const naab::governance::GovernanceRules& rules,
    naab::security::SandboxConfig& config)
{
    // Network disabled in governance → remove from sandbox
    if (!rules.network_allowed) {
        config.network_enabled = false;
        config.capabilities.erase(naab::security::Capability::NET_CONNECT);
    }
    // Shell disabled in governance → remove exec from sandbox
    if (!rules.shell_allowed) {
        config.allow_exec = false;
        config.capabilities.erase(naab::security::Capability::SYS_EXEC);
    }
    // Filesystem mode restrictions
    if (rules.capabilities.filesystem.mode == "none") {
        config.capabilities.erase(naab::security::Capability::FS_READ);
        config.capabilities.erase(naab::security::Capability::FS_WRITE);
    } else if (rules.capabilities.filesystem.mode == "read") {
        config.capabilities.erase(naab::security::Capability::FS_WRITE);
    }
    // Env vars disabled → remove from sandbox
    if (!rules.capabilities.env_vars.read) {
        config.capabilities.erase(naab::security::Capability::SYS_ENV);
    }
}

// Phase 7c: Initialize language registry with available executors
void initialize_executors() {
    auto& registry = naab::runtime::LanguageRegistry::instance();

    // === Cross-platform executors (work on both POSIX and Windows/MinGW64) ===

    // JavaScript via embedded QuickJS (compiled on MinGW64, skipped on MSVC)
#ifdef HAVE_QUICKJS
    registry.registerExecutor("javascript",
        std::make_unique<naab::runtime::JsExecutorAdapter>());
#endif

    // Subprocess Python (when embedded Python/pybind11 not available)
    // GenericSubprocessExecutor delegates to subprocess_helpers which has
    // a Windows CreateProcess implementation — works on all platforms.
#ifndef HAVE_PYBIND11
    registry.registerExecutor("python",
        std::make_unique<naab::runtime::GenericSubprocessExecutor>("python", "python3 {}", ".py"));
#endif

    // === POSIX-only executors (use fork/execvp/mkdtemp/dlfcn) ===
#ifndef _WIN32

    // C++ executor (dlfcn.h, mkdtemp)
    registry.registerExecutor("cpp",
        std::make_unique<naab::runtime::CppExecutorAdapter>());

    // Embedded Python (pybind11 — uses POSIX resource limits for containment)
#ifdef HAVE_PYBIND11
    naab::runtime::PythonInterpreterManager::initialize();
    registry.registerExecutor("python",
        std::make_unique<naab::runtime::PyExecutorAdapter>());
    naab::polyglot::initializePolyglotThreadPool();
#endif

    // Rust executor (Phase 3.1-3.3)
#ifdef HAVE_RUST
    registry.registerExecutor("rust",
        std::make_unique<naab::runtime::RustExecutor>());
#endif

    // Persistent shell executors (fork/pipe/execvp — state persists between blocks)
    registry.registerExecutor("shell",
        std::make_unique<naab::runtime::PersistentShellExecutor>());
    registry.registerExecutor("sh",
        std::make_unique<naab::runtime::PersistentShellExecutor>());
    registry.registerExecutor("bash",
        std::make_unique<naab::runtime::PersistentShellExecutor>());

    // Persistent Ruby executor
    registry.registerExecutor("ruby",
        std::make_unique<naab::runtime::PersistentRubyExecutor>());

    // Persistent Node.js executor (separate from QuickJS "javascript")
    registry.registerExecutor("node",
        std::make_unique<naab::runtime::NodePersistentExecutor>());

    // Go, Nim, Zig, Julia, C# (all use mkdtemp/chmod/unistd)
    registry.registerExecutor("go", std::make_unique<naab::runtime::GoExecutor>());
    registry.registerExecutor("golang", std::make_unique<naab::runtime::GoExecutor>());
    registry.registerExecutor("nim", std::make_unique<naab::runtime::NimExecutor>());
    registry.registerExecutor("zig", std::make_unique<naab::runtime::ZigExecutor>());
    registry.registerExecutor("julia", std::make_unique<naab::runtime::JuliaExecutor>());
    registry.registerExecutor("csharp", std::make_unique<naab::runtime::CSharpExecutor>());
    registry.registerExecutor("cs", std::make_unique<naab::runtime::CSharpExecutor>());

    // PHP, TypeScript (subprocess-based, command format assumes POSIX paths)
    registry.registerExecutor("php",
        std::make_unique<naab::runtime::GenericSubprocessExecutor>("php", "php {}", ".php"));
    registry.registerExecutor("typescript",
        std::make_unique<naab::runtime::GenericSubprocessExecutor>("typescript", "tsx {}", ".ts"));
    registry.registerExecutor("ts",
        std::make_unique<naab::runtime::GenericSubprocessExecutor>("ts", "tsx {}", ".ts"));
#endif // !_WIN32
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
    fmt::print("  naab-lang init --package            Create package project structure\n");
    fmt::print("  naab-lang manifest check            Validate naab.toml\n");
    fmt::print("  naab-lang calibrate                 Benchmark installed languages\n");
    fmt::print("  naab-lang race <file> --block N     Race polyglot block alternatives\n");
    fmt::print("  naab-lang --scan <path> [--language <lang>]  Run code quality scanner\n");
    fmt::print("  naab-lang version                   Show version\n");
    fmt::print("  naab-lang help                      Show this help\n");
    fmt::print("\nPackage Management:\n");
    fmt::print("  naab-lang install <user/repo>       Install package from GitHub\n");
    fmt::print("  naab-lang install <user/repo@ver>   Install specific version\n");
    fmt::print("  naab-lang install                   Install all from naab.toml\n");
    fmt::print("  naab-lang remove <package>          Remove installed package\n");
    fmt::print("  naab-lang update [package]          Update packages (all or specific)\n");
    fmt::print("  naab-lang list                      List installed packages\n");
    fmt::print("  naab-lang info <user/repo>          Show package info\n");
    fmt::print("  naab-lang search <query>            Search package registry\n");
    fmt::print("  naab-lang publish                   Validate package for publishing\n");
    fmt::print("\n");
    fmt::print("Options:\n");
    fmt::print("  --verbose, -v                       Enable verbose output\n");
    fmt::print("  --profile, -p                       Enable performance profiling\n");
    fmt::print("  --explain                           Explain execution step-by-step\n");
    fmt::print("  --debug, -d                         Enable interactive debugger\n");
    fmt::print("  --tree-walk                         Use tree-walk interpreter instead of VM\n");
    fmt::print("  --no-color                          Disable colored error messages\n");
    fmt::print("  --pipe                              Pipe mode: io.write() → stderr,\n");
    fmt::print("                                      io.output() → stdout (for JSON)\n");
    fmt::print("\nGovernance Options:\n");
    fmt::print("  --no-governance                     Completely disable governance engine\n");
    fmt::print("  --governance-override               Override soft-mandatory governance rules\n");
    fmt::print("  --governance-verbose               Show detailed governance check results\n");
    fmt::print("  --governance-report <path>          Write JSON governance report to file\n");
    fmt::print("  --governance-sarif <path>           Write SARIF governance report to file\n");
    fmt::print("  --governance-junit <path>           Write JUnit governance report to file\n");
    fmt::print("  --governance-record-baselines       Record output baselines for regression detection\n");
    fmt::print("  --governance-check-baselines        Check outputs against baselines (hard enforcement)\n");
    fmt::print("  --governance-dashboard              Print agent governance summary after execution\n");
    fmt::print("  --agent-id <name>                  Set agent identity for telemetry and role enforcement\n");
    fmt::print("  --governance-telemetry <path>       Write telemetry events to JSONL file (append mode)\n");
    fmt::print("  --governance-baseline-save          Save current governance results as baseline\n");
    fmt::print("  --env <name>                        Apply environment overrides from govern.json\n");
    fmt::print("\nSecurity Options:\n");
    fmt::print("  --sandbox-level <level>             Security: restricted|standard|elevated|unrestricted\n");
    fmt::print("                                      (default: unrestricted)\n");
    fmt::print("  --timeout <seconds>                 Execution timeout per block (default: 30)\n");
    fmt::print("  --memory-limit <MB>                 Memory limit per block (default: 512)\n");
    fmt::print("  --allow-network                     Enable network access (default: disabled)\n");
}

#ifdef _WIN32
// Ctrl-C handler: on first Ctrl-C, request graceful shutdown so the
// subprocess polling loop in subprocess_helpers.cpp can tear children down
// cleanly (exit code 124). On second Ctrl-C, return FALSE so the default
// handler terminates the process — the Job Object (KILL_ON_JOB_CLOSE) then
// guarantees every descendant dies with us.
static std::atomic<int> g_naab_ctrlc_count{0};
static BOOL WINAPI naabCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        if (g_naab_ctrlc_count.fetch_add(1) == 0) {
            naab::security::ResourceLimiter::requestShutdown();
            return TRUE;
        }
        return FALSE;
    }
    // CTRL_CLOSE_EVENT / CTRL_LOGOFF_EVENT / CTRL_SHUTDOWN_EVENT:
    // let the default handler run — Job Object tears the tree down.
    return FALSE;
}
#endif

// --- Key authorization helpers for --trust-key / --sign-key ---

// Scan argv[from..argc-1] for "--flag value" pair, return value or "".
static std::string findFlagValue(const std::string& flag,
                                 int argc, char** argv, int from) {
    for (int i = from; i + 1 < argc; ++i) {
        if (flag == argv[i]) return argv[i + 1];
    }
    return "";
}

// Verify that pub_pem was signed by an existing trusted key.
// sig_path is a file containing a base64 Ed25519 signature of pub_pem content.
static bool verifyKeyAuthorization(const std::string& pub_pem,
                                   const std::string& sig_path) {
    // Read sig file (bounded: authorization sigs are ~88 bytes base64)
    std::ifstream sf(sig_path);
    if (!sf.is_open()) {
        fprintf(stderr, "Error: Cannot read authorization signature: %s\n", sig_path.c_str());
        return false;
    }
    // Cap at 4KB
    std::string sig_b64;
    sig_b64.resize(4096);
    sf.read(&sig_b64[0], 4096);
    auto n = sf.gcount();
    sig_b64.resize(static_cast<size_t>(n));
    sf.close();
    // Trim whitespace
    while (!sig_b64.empty() && (sig_b64.back() == '\n' || sig_b64.back() == '\r'
                                || sig_b64.back() == ' '))
        sig_b64.pop_back();

    if (sig_b64.empty()) {
        fprintf(stderr, "Error: Authorization signature file is empty\n");
        return false;
    }

    // Verify against each trusted key
    auto keys = naab::security::TrustStore::loadKeys();
    for (const auto& [fingerprint, pem] : keys) {
        if (naab::security::CryptoUtils::ed25519Verify(pub_pem, sig_b64, pem)) {
            fprintf(stderr, "Key authorized by trusted key: %s\n", fingerprint.c_str());
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    // Enable ANSI colours + UTF-8 console on Windows; no-op on POSIX.
    naab::platform::enableAnsiConsole();

#ifdef _WIN32
    ::SetConsoleCtrlHandler(naabCtrlHandler, TRUE);
#endif

    // Export interpreter path so polyglot scripts can find naab-lang
    // This solves the common problem of Python/shell scripts not knowing
    // where the NAAb interpreter is located
    {
#ifdef _WIN32
        // On Windows, GetModuleFileName is more reliable than argv[0].
        std::string exe_str = naab::platform::executablePath();
        if (!exe_str.empty()) {
            naab::platform::setenv("NAAB_INTERPRETER_PATH", exe_str, true);
            std::filesystem::path exe_path(exe_str);
            naab::platform::setenv("NAAB_LANGUAGE_DIR",
                exe_path.parent_path().parent_path().string(), true);
        } else {
            naab::platform::setenv("NAAB_INTERPRETER_PATH", argv[0], true);
        }
#else
        std::error_code ec;
        auto exe_path = std::filesystem::canonical(argv[0], ec);
        if (!ec) {
            naab::platform::setenv("NAAB_INTERPRETER_PATH", exe_path.string(), true);
            naab::platform::setenv("NAAB_LANGUAGE_DIR",
                exe_path.parent_path().parent_path().string(), true);
        } else {
            // Fallback: use argv[0] as-is
            naab::platform::setenv("NAAB_INTERPRETER_PATH", argv[0], true);
        }
#endif
    }

    // Phase 7c: Initialize language executors
    initialize_executors();

    if (argc < 2) {
        // No arguments — start REPL
        return naab::repl::run();
    }

    // Pre-scan for global flags that can appear before the command
    // e.g., `naab-lang --pipe script.naab` or `naab-lang -v script.naab`
    bool global_pipe_mode = false;
    bool global_governance_override = false;
    std::string global_override_reason;
    bool global_no_governance = false;
    // V-GOV-007: fail-closed by default — require govern.json unless explicitly opted out.
    // Use --no-governance to run without a governance file (must be explicit).
    bool global_require_governance = true;
    bool global_governance_verbose = false;
    bool global_verbose = false;
    bool global_profile = false;
    bool global_explain = false;
    bool global_debug = false;
    bool global_no_color = false;
    bool global_quiet = false;
    bool global_strict_types = false;
    bool global_use_vm = true;  // VM is default since Phase 16
    bool global_governance_dashboard = false;
    std::string global_agent_id = "anonymous";
    std::string global_governance_telemetry;
    bool global_governance_baseline_save = false;
    bool global_drift_baseline_save = false;
    std::string global_governance_env;
    std::string global_sandbox_level = "unrestricted";  // Default: full language power
    int command_arg_index = 1;  // Index of the actual command/file in argv
    static size_t global_gc_threshold = 5000;
    static bool   global_gc_stats     = false;
    bool global_lock_update    = false;   // --lock: write/update naab.lock
    bool global_lock_check     = false;   // --lock-check: fail on runtime drift
    std::string global_lock_path;         // --lock-path: override lockfile location
    bool global_lint_only      = false;   // --lint-only: parse+governance, skip execution (V-LSP-005)
    unsigned int global_timeout = 0;       // --timeout: override default 30s (0 = use default)

    while (command_arg_index < argc) {
        std::string arg(argv[command_arg_index]);
        if (arg == "--vm") {
            global_use_vm = true;
            command_arg_index++;
        } else if (arg == "--tree-walk") {
            global_use_vm = false;
            command_arg_index++;
        } else if (arg == "--pipe") {
            global_pipe_mode = true;
            command_arg_index++;
        } else if (arg == "--governance-override") {
            global_governance_override = true;
            command_arg_index++;
        } else if (arg == "--override-reason" && command_arg_index + 1 < argc) {
            global_override_reason = argv[++command_arg_index];
            command_arg_index++;
        } else if (arg == "--no-governance") {
            global_no_governance = true;
            command_arg_index++;
        } else if (arg == "--require-governance") {
            global_require_governance = true;
            command_arg_index++;
        } else if (arg == "--governance-verbose") {
            global_governance_verbose = true;
            command_arg_index++;
        } else if (arg == "--governance-dashboard") {
            global_governance_dashboard = true;
            command_arg_index++;
        } else if (arg == "--verbose" || arg == "-v") {
            global_verbose = true;
            command_arg_index++;
        } else if (arg == "--profile" || arg == "-p") {
            global_profile = true;
            command_arg_index++;
        } else if (arg == "--explain") {
            global_explain = true;
            command_arg_index++;
        } else if (arg == "--debug" || arg == "-d") {
            global_debug = true;
            command_arg_index++;
        } else if (arg == "--no-color") {
            global_no_color = true;
            command_arg_index++;
        } else if (arg == "--quiet" || arg == "-q") {
            global_quiet = true;
            command_arg_index++;
        } else if (arg == "--strict-types" || arg == "--strict") {
            global_strict_types = true;
            command_arg_index++;
        } else if (arg == "--agent-id" && command_arg_index + 1 < argc) {
            global_agent_id = argv[++command_arg_index];
            command_arg_index++;
        } else if (arg == "--governance-telemetry" && command_arg_index + 1 < argc) {
            global_governance_telemetry = argv[++command_arg_index];
            command_arg_index++;
        } else if (arg == "--governance-baseline-save") {
            global_governance_baseline_save = true;
            command_arg_index++;
        } else if (arg == "--drift-baseline-save") {
            global_drift_baseline_save = true;
            command_arg_index++;
        } else if (arg == "--env" && command_arg_index + 1 < argc) {
            global_governance_env = argv[++command_arg_index];
            command_arg_index++;
        } else if (arg == "--gc-threshold" && command_arg_index + 1 < argc) {
            try { global_gc_threshold = std::stoul(argv[command_arg_index + 1]); }
            catch (...) { global_gc_threshold = 5000; }
            command_arg_index += 2;
        } else if (arg == "--gc-stats") {
            global_gc_stats = true;
            command_arg_index++;
        } else if (arg == "--lock") {
            global_lock_update = true;
            command_arg_index++;
        } else if (arg == "--lock-check") {
            global_lock_check = true;
            command_arg_index++;
        } else if (arg == "--lock-path" && command_arg_index + 1 < argc) {
            global_lock_path = argv[++command_arg_index];
            command_arg_index++;
        } else if (arg == "--sandbox-level" && command_arg_index + 1 < argc) {
            global_sandbox_level = argv[++command_arg_index];
            command_arg_index++;
        } else if (arg == "--timeout" && command_arg_index + 1 < argc) {
            try { global_timeout = std::stoi(argv[++command_arg_index]); }
            catch (...) { global_timeout = 0; }
            command_arg_index++;
        } else if (arg == "--lint-only") {
            // V-LSP-005: parse + governance pre-flight without executing user code
            global_lint_only = true;
            command_arg_index++;
        } else if (arg == "--signing-key" && command_arg_index + 1 < argc) {
            // V-SC-009: Override NAAB_SIGNING_KEY for this invocation
            naab::platform::setenv("NAAB_SIGNING_KEY", argv[++command_arg_index], true);
            command_arg_index++;
        } else if (arg == "--keygen") {
            // V-SC-009: Generate Ed25519 keypair — does NOT auto-install to trust store.
            // The operator must explicitly run --trust-key to authorize the public key.
            std::string private_pem, public_pem;
            if (!naab::security::CryptoUtils::ed25519Keygen(private_pem, public_pem)) {
                fprintf(stderr, "Error: Ed25519 key generation failed\n");
                return 1;
            }
            // Determine output path for private key
            std::string priv_path = "naab-signing-key.pem";
            if (command_arg_index + 1 < argc && argv[command_arg_index + 1][0] != '-') {
                priv_path = argv[++command_arg_index];
            }
            std::string pub_path = priv_path + ".pub";

            // Write private key — born with 0600 permissions (no world-readable window)
            if (!naab::security::writeFileSecure(priv_path, private_pem, 0600)) {
                fprintf(stderr, "Error: Failed to write private key to %s\n", priv_path.c_str());
                return 1;
            }
            // Write public key to separate file
            if (!naab::security::writeFileSecure(pub_path, public_pem, 0644)) {
                fprintf(stderr, "Error: Failed to write public key to %s\n", pub_path.c_str());
                return 1;
            }

            std::string fp = naab::security::CryptoUtils::ed25519Fingerprint(public_pem);
            fprintf(stderr, "Ed25519 keypair generated:\n"
                            "  Private key: %s (keep secret — never commit!)\n"
                            "  Public key:  %s\n"
                            "  Fingerprint: %s\n\n"
                            "Key is NOT yet trusted. To install:\n"
                            "  naab-lang --trust-key %s\n\n"
                            "To sign governance files after installing:\n"
                            "  export NAAB_SIGNING_KEY=%s\n"
                            "  naab-lang --sign-governance\n",
                    priv_path.c_str(), pub_path.c_str(),
                    fp.c_str(), pub_path.c_str(), priv_path.c_str());
            return 0;
        } else if (arg == "--trust-key" && command_arg_index + 1 < argc) {
            // V-SC-009: Install a public key PEM into the trust store.
            // When the trust store is non-empty (non-bootstrap), requires --authorized-by
            // with a signature from an existing trusted key (countersigning).
            std::string pub_path = argv[++command_arg_index];
            std::ifstream pf(pub_path);
            if (!pf.is_open()) {
                fprintf(stderr, "Error: Cannot read public key file: %s\n", pub_path.c_str());
                return 1;
            }
            std::string pub_pem((std::istreambuf_iterator<char>(pf)),
                                 std::istreambuf_iterator<char>());
            pf.close();

            // Countersigning gate: non-empty trust store requires authorization
            if (naab::security::TrustStore::hasKeys()) {
                std::string sig_path = findFlagValue(
                    "--authorized-by", argc, argv, command_arg_index + 1);
                if (sig_path.empty()) {
                    fprintf(stderr,
                        "Error: Trust store is non-empty — new key requires authorization.\n"
                        "  An existing key holder must authorize this key first.\n\n"
                        "  Then install with:\n"
                        "    naab-lang --trust-key %s --authorized-by <sig-file>\n",
                        pub_path.c_str());
                    return 1;
                }
                if (!verifyKeyAuthorization(pub_pem, sig_path)) {
                    fprintf(stderr,
                        "Error: Key authorization signature invalid or not from a trusted key.\n");
                    return 1;
                }
            }

            // Bootstrap (empty store) or verified — install
            if (!naab::security::TrustStore::installKey(pub_pem)) {
                fprintf(stderr, "Error: Failed to install key (invalid PEM or I/O error)\n");
                return 1;
            }
            std::string fp = naab::security::CryptoUtils::ed25519Fingerprint(pub_pem);
            fprintf(stderr, "Trusted key installed: %s (fingerprint: %s)\n",
                    pub_path.c_str(), fp.c_str());
            return 0;
        } else if (arg == "--sign-key" && command_arg_index + 1 < argc) {
            // Authorize a new public key by signing it with an existing trusted private key.
            // Creates a .sig file that can be passed to --trust-key --authorized-by.
            std::string pub_path = argv[++command_arg_index];
            std::ifstream pkf(pub_path);
            if (!pkf.is_open()) {
                fprintf(stderr, "Error: Cannot read public key file: %s\n", pub_path.c_str());
                return 1;
            }
            std::string pub_pem((std::istreambuf_iterator<char>(pkf)),
                                 std::istreambuf_iterator<char>());
            pkf.close();

            // Read private key from NAAB_SIGNING_KEY
            std::string private_pem =
                naab::governance::GovernanceEngine::readSigningKeyForCLI();
            if (private_pem.empty()) {
                fprintf(stderr,
                    "Error: No signing key configured.\n"
                    "  Set NAAB_SIGNING_KEY to an existing trusted private key.\n");
                return 1;
            }

            // Sign the public key content
            std::string sig_b64 =
                naab::security::CryptoUtils::ed25519Sign(pub_pem, private_pem);
            if (sig_b64.empty()) {
                fprintf(stderr, "Error: Failed to sign public key\n");
                return 1;
            }

            // Write sig to pub_path + ".sig"
            std::string sig_path = pub_path + ".sig";
            if (!naab::security::writeFileSecure(sig_path, sig_b64 + "\n", 0644)) {
                fprintf(stderr, "Error: Failed to write authorization signature to %s\n",
                        sig_path.c_str());
                return 1;
            }

            // Get signer fingerprint for display
            std::string signer_fp =
                naab::governance::GovernanceEngine::getKeyFingerprint();
            fprintf(stderr, "Authorization signature written: %s\n"
                            "  Signed by: %s\n\n"
                            "  Install with:\n"
                            "    naab-lang --trust-key %s --authorized-by %s\n",
                    sig_path.c_str(), signer_fp.c_str(),
                    pub_path.c_str(), sig_path.c_str());
            return 0;
        } else if (arg == "--list-keys") {
            // V-SC-009: List installed trusted keys
            auto fingerprints = naab::security::TrustStore::listKeyFingerprints();
            if (fingerprints.empty()) {
                fprintf(stderr, "No trusted keys installed. Trust store: %s\n",
                        naab::security::TrustStore::getStorePath().c_str());
            } else {
                fprintf(stdout, "Trusted keys in %s:\n",
                        naab::security::TrustStore::getStorePath().c_str());
                for (const auto& fp : fingerprints) {
                    fprintf(stdout, "  %s\n", fp.c_str());
                }
            }
            return 0;
        } else if (arg == "--key-info") {
            // Authority Decay: show key metadata
            if (command_arg_index + 1 >= argc) {
                fprintf(stderr, "Error: --key-info requires a fingerprint argument\n");
                return 1;
            }
            std::string fp = argv[++command_arg_index];
            auto meta = naab::security::TrustStore::loadKeyMetadata(fp);
            fprintf(stdout, "Key: %s\n", fp.c_str());
            if (!meta.label.empty())
                fprintf(stdout, "  Label:      %s\n", meta.label.c_str());
            if (meta.created_at > 0) {
                char buf[64];
                time_t t = static_cast<time_t>(meta.created_at);
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", gmtime(&t));
                fprintf(stdout, "  Created:    %s\n", buf);
            }
            if (meta.expires_at > 0) {
                char buf[64];
                time_t t = static_cast<time_t>(meta.expires_at);
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", gmtime(&t));
                fprintf(stdout, "  Expires:    %s%s\n", buf,
                        naab::security::TrustStore::isKeyExpired(meta) ? " (EXPIRED)" : "");
            } else {
                fprintf(stdout, "  Expires:    never\n");
            }
            fprintf(stdout, "  Revoked:    %s\n", meta.revoked ? "YES" : "no");
            if (meta.revoked && !meta.revoked_reason.empty())
                fprintf(stdout, "  Reason:     %s\n", meta.revoked_reason.c_str());
            return 0;
        } else if (arg == "--revoke-key") {
            // Authority Decay: revoke a trusted key
            if (command_arg_index + 1 >= argc) {
                fprintf(stderr, "Error: --revoke-key requires a fingerprint argument\n");
                return 1;
            }
            std::string fp = argv[++command_arg_index];
            std::string reason = "revoked via CLI";
            if (command_arg_index + 1 < argc && argv[command_arg_index + 1][0] != '-') {
                reason = argv[++command_arg_index];
            }
            if (!naab::security::TrustStore::revokeKey(fp, reason)) {
                fprintf(stderr, "Error: Failed to revoke key %s\n", fp.c_str());
                return 1;
            }
            fprintf(stderr, "Key revoked: %s (reason: %s)\n", fp.c_str(), reason.c_str());
            return 0;
        } else if (arg == "--approve" && command_arg_index + 1 < argc) {
            // Generate a signed approval token for a governance rule
            std::string rule_name = argv[++command_arg_index];
            std::string reason = "manual approval";
            int expiry_hours = 24;
            // Parse optional --reason and --expiry
            for (int j = command_arg_index + 1; j < argc; ++j) {
                std::string jarg = argv[j];
                if (jarg == "--reason" && j + 1 < argc) {
                    reason = argv[++j]; command_arg_index = j;
                } else if (jarg == "--expiry" && j + 1 < argc) {
                    try { expiry_hours = std::stoi(argv[++j]); } catch (const std::exception&) {
                        fprintf(stderr, "Error: --expiry requires a numeric value\n"); return 1;
                    }
                    command_arg_index = j;
                }
            }
            // Load signing key
            const char* key_env = std::getenv("NAAB_SIGNING_KEY");
            if (!key_env) {
                fprintf(stderr, "Error: NAAB_SIGNING_KEY not set\n"
                    "  Set it to your Ed25519 private key path:\n"
                    "  export NAAB_SIGNING_KEY=~/.naab/keys/signing.pem\n");
                return 1;
            }
            std::ifstream kf(key_env);
            if (!kf.is_open()) {
                fprintf(stderr, "Error: Cannot read signing key: %s\n", key_env);
                return 1;
            }
            std::string pem((std::istreambuf_iterator<char>(kf)),
                             std::istreambuf_iterator<char>());
            kf.close();
            // Compute fields
            std::string approver_id = naab::security::CryptoUtils::ed25519Fingerprint(pem);
            int64_t expiry = static_cast<int64_t>(time(nullptr)) +
                static_cast<int64_t>(expiry_hours) * 3600;
            // Canonical encoding — must match hasValidApproval() format exactly
            auto lpEncode = [](const std::string& s) -> std::string {
                return std::to_string(s.size()) + ":" + s;
            };
            std::string canonical = lpEncode(rule_name) + lpEncode(approver_id)
                + lpEncode(reason) + std::to_string(expiry);
            std::string signature = naab::security::CryptoUtils::ed25519Sign(canonical, pem);
            // Build token
            nlohmann::json token;
            token["approver_id"] = approver_id;
            token["reason"] = reason;
            token["expiry_timestamp"] = expiry;
            token["signature"] = signature;
            // Read or create approvals store
            std::string store_path = ".naab/approvals.json";
            nlohmann::json store = nlohmann::json::object();
            {
                std::ifstream sf(store_path);
                if (sf.is_open()) {
                    try { sf >> store; } catch (...) {}
                }
            }
            store[rule_name] = token;
            // Ensure .naab directory exists and write securely
            std::filesystem::create_directories(".naab");
            naab::security::writeFileSecure(store_path, store.dump(2), 0600);
            fprintf(stderr, "Approval granted:\n"
                "  Rule:     %s\n"
                "  Approver: %s\n"
                "  Reason:   %s\n"
                "  Expires:  %d hours\n",
                rule_name.c_str(), approver_id.c_str(),
                reason.c_str(), expiry_hours);
            return 0;
        } else if (arg == "--list-approvals") {
            // List current approval tokens
            std::string store_path = ".naab/approvals.json";
            std::ifstream sf(store_path);
            if (!sf.is_open()) {
                fprintf(stderr, "No approvals found (%s does not exist).\n",
                    store_path.c_str());
                return 0;
            }
            nlohmann::json store;
            try { sf >> store; } catch (...) {
                fprintf(stderr, "Error: Invalid approvals file: %s\n",
                    store_path.c_str());
                return 1;
            }
            sf.close();
            if (store.empty()) {
                fprintf(stderr, "No approvals stored.\n");
                return 0;
            }
            int64_t now = static_cast<int64_t>(time(nullptr));
            fprintf(stdout, "Approval tokens in %s:\n", store_path.c_str());
            for (auto& [rule, tok] : store.items()) {
                int64_t exp = tok.value("expiry_timestamp", int64_t(0));
                bool expired = (exp > 0 && now > exp);
                fprintf(stdout, "  %s %s\n"
                    "    approver: %s\n"
                    "    reason:   %s\n",
                    expired ? "[EXPIRED]" : "[VALID]  ", rule.c_str(),
                    tok.value("approver_id", std::string("unknown")).c_str(),
                    tok.value("reason", std::string("")).c_str());
                if (exp > 0) {
                    char buf[64];
                    time_t t = static_cast<time_t>(exp);
                    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", gmtime(&t));
                    fprintf(stdout, "    expires:  %s%s\n", buf,
                        expired ? " (EXPIRED)" : "");
                } else {
                    fprintf(stdout, "    expires:  never\n");
                }
            }
            return 0;
        } else if (arg == "--revoke-approval" && command_arg_index + 1 < argc) {
            // Remove an approval token
            std::string rule_name = argv[++command_arg_index];
            std::string store_path = ".naab/approvals.json";
            std::ifstream sf(store_path);
            if (!sf.is_open()) {
                fprintf(stderr, "No approvals file found: %s\n", store_path.c_str());
                return 1;
            }
            nlohmann::json store;
            try { sf >> store; } catch (...) {
                fprintf(stderr, "Error: Invalid approvals file\n");
                return 1;
            }
            sf.close();
            if (!store.contains(rule_name)) {
                fprintf(stderr, "No approval for rule: %s\n", rule_name.c_str());
                return 1;
            }
            store.erase(rule_name);
            naab::security::writeFileSecure(store_path, store.dump(2), 0600);
            fprintf(stderr, "Approval revoked: %s\n", rule_name.c_str());
            return 0;
        } else if (arg == "--attest") {
            // Environment Attestation: verify prerequisites from govern.json
            naab::governance::GovernanceEngine gov;
            gov.discoverAndLoad(".");
            if (!gov.isActive()) {
                fprintf(stderr, "No govern.json found or governance is disabled\n");
                return 4;
            }
            auto results = gov.runAttestation();
            if (results.empty()) {
                fprintf(stdout, "No prerequisites configured\n");
                return 0;
            }
            bool all_passed = true;
            for (const auto& r : results) {
                const char* status = r.passed ? "PASS" : "FAIL";
                fprintf(stdout, "  [%s] %s:%s — required: %s, observed: %s\n",
                        status, r.check_type.c_str(), r.check_name.c_str(),
                        r.required.c_str(), r.observed.c_str());
                if (!r.passed) all_passed = false;
            }
            fprintf(stdout, "\nAttestation: %s (%zu checks)\n",
                    all_passed ? "PASSED" : "FAILED", results.size());
            return all_passed ? 0 : 3;
        } else if (arg == "--sign-governance") {
            // Sign govern.json (Ed25519 via NAAB_SIGNING_KEY, or legacy HMAC via NAAB_GOVERN_KEY)
            naab::governance::GovernanceEngine gov;
            std::string gov_path;
            if (command_arg_index + 1 < argc && argv[command_arg_index + 1][0] != '-') {
                gov_path = argv[++command_arg_index];
            } else {
                // Discover govern.json from CWD
                gov.discoverAndLoad(".");
                gov_path = gov.getLoadedPath();
            }
            if (gov_path.empty()) {
                fprintf(stderr, "Error: No govern.json found to sign\n");
                return 4;
            }
            return naab::governance::GovernanceEngine::signFile(gov_path) ? 0 : 1;
        } else if (arg == "--sign-baseline") {
            // Sign drift baseline (Ed25519 via NAAB_SIGNING_KEY, or legacy HMAC via NAAB_GOVERN_KEY)
            naab::governance::GovernanceEngine gov;
            gov.discoverAndLoad(".");
            std::string bp = gov.resolveDriftBaselinePath();
            if (bp.empty()) {
                fprintf(stderr, "Error: No drift baseline path configured\n");
                return 4;
            }
            return naab::governance::GovernanceEngine::signFile(bp) ? 0 : 1;
        } else if (arg == "--") {
            // V-LSP-004: end-of-flags sentinel — everything after is positional
            command_arg_index++;
            break;
        } else if (arg == "--repl") {
            return naab::repl::run(global_no_governance);
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

    // Handle --scan command
    if (command == "--scan" || command == "scan") {
        std::string target_path;
        std::string scan_language = "auto";

        for (int i = command_arg_index + 1; i < argc; ++i) {
            std::string arg(argv[i]);
            if (arg == "--language" && i + 1 < argc) {
                scan_language = argv[++i];
            } else if (target_path.empty()) {
                target_path = arg;
            } else if (scan_language == "auto") {
                // Allow: naab-lang --scan <path> <language>
                scan_language = arg;
            }
        }

        if (target_path.empty()) {
            fmt::print("Usage: naab-lang --scan <path> [language|auto]\n");
            fmt::print("Languages: python, javascript, cpp, go, rust, naab, auto\n");
            fflush(stdout);
            return 1;
        }

        naab::scanner::ScannerEngine engine;
        engine.loadConfig(target_path);
        auto result = engine.scan(target_path, scan_language);
        auto report = engine.formatTextReport(result);
        fmt::print("{}\n", report);
        engine.saveReports(result);

        // Exit code: fail on hard violations
        int hard_count = 0;
        for (const auto& issue : result.issues) {
            if (issue.level == "hard") hard_count++;
        }
        if (hard_count > 0) {
            fmt::print("\nRESULT: FAIL ({} hard violations)\n", hard_count);
            fflush(stdout);
            return 1;
        }
        fmt::print("\nRESULT: PASS (no hard violations)\n");
        fflush(stdout);
        return 0;
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
        bool verbose = global_verbose;
        bool profile = global_profile;
        bool explain = global_explain;
        bool no_color = global_no_color;
        bool debug = global_debug;
        bool pipe_mode = global_pipe_mode;  // Inherit from global pre-scan
        bool governance_override = global_governance_override;
        std::string override_reason = global_override_reason;
        bool no_governance = global_no_governance;
        bool require_governance = global_require_governance;
        bool governance_verbose = global_governance_verbose;
        bool strict_types = global_strict_types;
        bool use_vm = global_use_vm;
        bool governance_dashboard = global_governance_dashboard;
        std::string agent_id = global_agent_id;
        std::string governance_telemetry_path = global_governance_telemetry;
        bool governance_record_baselines = false;
        bool governance_check_baselines = false;
        bool governance_baseline_save = global_governance_baseline_save;
        bool drift_baseline_save = global_drift_baseline_save;
        std::string governance_env = global_governance_env;
        size_t gc_threshold = global_gc_threshold;
        bool   gc_stats     = global_gc_stats;
        bool lock_update    = global_lock_update;
        bool lock_check     = global_lock_check;
        std::string lock_path = global_lock_path;
        bool lint_only      = global_lint_only;
        std::string governance_report_json;
        std::string governance_report_sarif;
        std::string governance_report_junit;
        std::string sandbox_level = global_sandbox_level;  // Inherit from global pre-scan
        unsigned int timeout = global_timeout > 0 ? global_timeout : 30;
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
                try { timeout = std::stoi(argv[++i]); } catch (const std::exception&) {
                    fprintf(stderr, "Error: --timeout requires a numeric value\n"); return 1;
                }
            } else if (arg == "--memory-limit" && i + 1 < argc) {
                try { memory_limit = std::stoull(argv[++i]); } catch (const std::exception&) {
                    fprintf(stderr, "Error: --memory-limit requires a numeric value\n"); return 1;
                }
            } else if (arg == "--allow-network") {
                network_enabled = true;
            } else if (arg == "--no-governance") {
                no_governance = true;
            } else if (arg == "--require-governance") {
                require_governance = true;
            } else if (arg == "--governance-override") {
                governance_override = true;
            } else if (arg == "--override-reason" && i + 1 < argc) {
                override_reason = argv[++i];
            } else if (arg == "--governance-report" && i + 1 < argc) {
                governance_report_json = argv[++i];
            } else if (arg == "--governance-sarif" && i + 1 < argc) {
                governance_report_sarif = argv[++i];
            } else if (arg == "--governance-junit" && i + 1 < argc) {
                governance_report_junit = argv[++i];
            } else if (arg == "--agent-id" && i + 1 < argc) {
                agent_id = argv[++i];
            } else if (arg == "--governance-telemetry" && i + 1 < argc) {
                governance_telemetry_path = argv[++i];
            } else if (arg == "--governance-verbose") {
                governance_verbose = true;
            } else if (arg == "--governance-dashboard") {
                governance_dashboard = true;
            } else if (arg == "--governance-record-baselines") {
                // Will be applied after governance loads
                governance_record_baselines = true;
            } else if (arg == "--governance-check-baselines") {
                governance_check_baselines = true;
            } else if (arg == "--governance-baseline-save") {
                governance_baseline_save = true;
            } else if (arg == "--drift-baseline-save") {
                drift_baseline_save = true;
            } else if (arg == "--env" && i + 1 < argc) {
                governance_env = argv[++i];
            } else if (arg == "--strict-types" || arg == "--strict") {
                strict_types = true;
            } else if (arg == "--vm") {
                use_vm = true;
            } else if (arg == "--tree-walk") {
                use_vm = false;
            } else if (arg == "--gc-threshold" && i + 1 < argc) {
                try { gc_threshold = std::stoul(argv[++i]); }
                catch (...) { gc_threshold = 5000; }
            } else if (arg == "--gc-stats") {
                gc_stats = true;
            } else if (arg == "--lock") {
                lock_update = true;
            } else if (arg == "--lock-check") {
                lock_check = true;
            } else if (arg == "--lock-path" && i + 1 < argc) {
                lock_path = argv[++i];
            } else if (arg == "--lint-only") {
                // V-LSP-005: parse + governance pre-flight without executing user code
                lint_only = true;
            } else if (arg == "--") {
                // V-LSP-004: end-of-flags — remaining args are positional
                while (++i < argc) {
                    if (filename.empty()) filename = argv[i];
                    else script_args.push_back(argv[i]);
                }
                break;
            } else if (arg == "--help" || arg == "-h") {
                print_usage();
                fflush(stdout);
                return 0;
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
                           "    --governance-verbose Show detailed governance check results\n"
                           "    --governance-report <path>  Write JSON governance report\n"
                           "    --governance-sarif <path>   Write SARIF governance report\n"
                           "    --governance-junit <path>   Write JUnit governance report\n"
                           "    --governance-record-baselines  Record output baselines\n"
                           "    --governance-check-baselines   Check baselines (hard enforcement)\n"
                           "    --governance-dashboard Print agent governance summary after run\n"
                           "    --agent-id <name>     Set agent identity for telemetry/roles\n"
                           "    --governance-telemetry <path>  Write telemetry JSONL\n"
                           "    --governance-baseline-save  Save governance baseline\n"
                           "    --env <name>          Apply environment overrides\n"
                           "    --strict-types        Abort on type errors (pre-execution check)\n"
                           "    --vm                  Use bytecode VM (default)\n"
                           "    --tree-walk           Use tree-walk interpreter instead of VM\n"
                           "    --gc-threshold <N>    GC collection threshold (default: 5000)\n"
                           "    --gc-stats            Print GC statistics after execution\n\n"
                           "  Note: There is no --path flag. NAAb resolves modules relative to\n"
                           "  the script's directory. To use modules from another location,\n"
                           "  place the script in or near the modules directory, or use\n"
                           "  absolute paths in 'use' statements.\n", arg);
                fflush(stdout);
                return 4;  // Config error exit code
            } else {
                // Non-flag argument
                if (filename.empty()) {
                    // First non-flag is the filename
                    filename = arg;
                } else {
                    // First script positional arg — from here, ALL remaining
                    // args belong to the script. This prevents NAAb flags
                    // (--profile, --verbose, etc.) from colliding with
                    // identically-named script flags.
                    script_args.push_back(arg);
                    for (++i; i < argc; ++i) {
                        script_args.push_back(argv[i]);
                    }
                    break;
                }
            }
        }

        // Validate filename was provided
        if (filename.empty()) {
            fmt::print("Error: Missing file argument. Usage: naab-lang run <file.naab>\n");
            return 1;
        }

        // FIX 22: Helpful error for common flags from other languages (-e, -c, --eval)
        if (!filename.empty() && filename[0] == '-' && filename != "--") {
            if (filename == "-e" || filename == "--eval" || filename == "--exec" ||
                filename == "-c" || filename == "--command") {
                fmt::print(stderr,
                    "Error: NAAb doesn't support inline code execution with '{}'.\n\n"
                    "  Help: Write your code to a file and run it:\n"
                    "    echo 'main {{ io.write(\"hello\") }}' > /tmp/eval.naab\n"
                    "    naab-lang /tmp/eval.naab\n\n"
                    "  Or use a process substitution:\n"
                    "    naab-lang <(echo 'main {{ io.write(\"hello\") }}')\n",
                    filename);
                fflush(stderr);
                return 1;
            }
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
            return 4;  // Config error exit code
        }

        // Apply CLI overrides to security config
        security_config.max_cpu_seconds = timeout;
        security_config.max_memory_mb = memory_limit;
        security_config.network_enabled = network_enabled;

        // Set default config for SandboxManager
        naab::security::SandboxManager::instance().setDefaultConfig(security_config);

        // Python import blocking: enforced at runtime in PythonCExecutor::executeWithReturn()
        // via __import__ hook that checks govern.json languages.python.imports.blocked

        if (verbose) {
            fmt::print("[Security] Sandbox level: {}, timeout: {}s, memory: {}MB, network: {}\n",
                       sandbox_level, timeout, memory_limit, network_enabled ? "enabled" : "disabled");
        }

        // Activate sandbox for entire script execution (not just polyglot blocks)
        // This makes ScopedSandbox::getCurrent() return non-null during stdlib calls
        naab::security::ScopedSandbox script_sandbox(security_config);

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

        // FIX 25: Resolve filename to absolute path for correct module resolution
        // When launched as background process, CWD may differ from script location
        {
            std::error_code ec;
            auto abs = std::filesystem::absolute(filename, ec);
            if (!ec) filename = abs.string();
        }

        // Load governance once — reused for both preflight and VM/tree-walker paths.
        // Loading once eliminates the TOCTOU window where govern.json could be swapped
        // between preflight validation and actual execution.
        naab::governance::GovernanceEngine shared_governance;
        auto gov_abs_file = std::filesystem::absolute(filename);
        auto gov_script_dir = gov_abs_file.parent_path();
        bool gov_loaded = shared_governance.discoverAndLoad(gov_script_dir.string());

        // Pre-flight authority — persists to VM-path check below
        bool _has_authority = false;

        // Pre-flight: blocked_flags enforcement for BOTH execution paths
        // Must run before interpreter.disableGovernance() which skips tree-walker governance entirely
        {

            if (!gov_loaded && naab::governance::g_governance_hard_block) {
                fprintf(stderr,
                    "[governance] INTEGRITY BLOCK: Governance configuration is tamper-protected.\n"
                    "  Execution is blocked. Only the project owner can authorize changes.\n");
                fflush(stderr);
                _exit(3);
            }

            if (gov_loaded && shared_governance.isActive()) {
                const char* _govern_key = std::getenv("NAAB_GOVERN_KEY");
                const char* _signing_key = std::getenv("NAAB_SIGNING_KEY");
                // F1: Validate key against project's signature — not just existence
                // _has_authority declared in outer scope for VM-path reuse
                {
                    std::string gov_dir = shared_governance.getGovernDir();
                    std::string sig_path = gov_dir + "/govern.json.sig";
                    std::string gov_path = gov_dir + "/govern.json";
                    std::error_code _ec;
                    if (!gov_dir.empty() && std::filesystem::exists(sig_path, _ec)) {
                        std::ifstream gf(gov_path);
                        std::ifstream sf(sig_path);
                        if (gf.is_open() && sf.is_open()) {
                            std::string content((std::istreambuf_iterator<char>(gf)),
                                                 std::istreambuf_iterator<char>());
                            std::string stored_sig((std::istreambuf_iterator<char>(sf)),
                                                    std::istreambuf_iterator<char>());
                            // Trim trailing newlines from sig
                            while (!stored_sig.empty() && (stored_sig.back() == '\n' || stored_sig.back() == '\r'))
                                stored_sig.pop_back();

                            if (_signing_key && *_signing_key) {
                                // Ed25519: read key file, verify sig
                                std::ifstream kf(_signing_key);
                                if (kf.is_open()) {
                                    std::string key_pem((std::istreambuf_iterator<char>(kf)),
                                                         std::istreambuf_iterator<char>());
                                    // Sig format: "ed25519:<base64>"
                                    if (stored_sig.rfind("ed25519:", 0) == 0) {
                                        std::string b64 = stored_sig.substr(8);
                                        _has_authority = naab::security::CryptoUtils::ed25519Verify(
                                            content, b64, key_pem);
                                    }
                                }
                            } else if (_govern_key && *_govern_key) {
                                // Legacy HMAC: verify key produces matching sig
                                std::string expected_hmac = naab::security::CryptoUtils::hmacSha256(
                                    content, std::string(_govern_key));
                                // Sig format: "hmac:<hex>" or raw hex (legacy)
                                std::string sig_value = stored_sig;
                                if (sig_value.rfind("hmac:", 0) == 0)
                                    sig_value = sig_value.substr(5);
                                _has_authority = naab::security::CryptoUtils::constantTimeCompare(
                                    expected_hmac, sig_value);
                            }
                        }
                    }
                    // No .sig file = unsigned project — key has nothing to validate against
                }
                if (!_has_authority) {
                    auto checkBlocked = [&](const std::string& flag, bool was_used) {
                        if (was_used && shared_governance.isBlockedFlag(flag)) {
                            fprintf(stderr,
                                "[governance] INTEGRITY BLOCK: flag '%s' is locked by the project owner.\n"
                                "  This flag is listed in integrity.blocked_flags in govern.json.\n"
                                "  Help: Remove '%s' from your command to proceed.\n"
                                "  The project owner has restricted this flag to prevent governance bypass.\n"
                                "  To modify governance settings, contact the project owner.\n",
                                flag.c_str(), flag.c_str());
                            naab::governance::g_governance_hard_block = true;
                        }
                    };
                    checkBlocked("--no-governance", no_governance);
                    checkBlocked("--tree-walk", !use_vm);
                    checkBlocked("--drift-baseline-save", drift_baseline_save);
                    checkBlocked("--governance-override", governance_override);
                    checkBlocked("--governance-baseline-save", governance_baseline_save);

                    // Gap 13/16: In enforce mode with a signed govern.json,
                    // --no-governance and --governance-override are implicitly blocked
                    // even without blocked_flags. This prevents LLMs from bypassing
                    // governance on projects that forgot to configure blocked_flags.
                    // Only applies when govern.json.sig exists (signed = project owner
                    // cares about integrity). Unsigned configs remain permissive for dev/test.
                    if (shared_governance.getMode() == naab::governance::GovernanceMode::ENFORCE) {
                        std::string gov_dir = shared_governance.getGovernDir();
                        bool config_is_signed = !gov_dir.empty() &&
                            std::filesystem::exists(gov_dir + "/govern.json.sig");
                        if (config_is_signed) {
                            auto implicitBlock = [&](const std::string& flag, bool was_used) {
                                if (was_used && !shared_governance.isBlockedFlag(flag)) {
                                    fprintf(stderr,
                                        "[governance] INTEGRITY BLOCK: flag '%s' is not allowed.\n"
                                        "  This project's govern.json is signed — bypass flags are\n"
                                        "  implicitly blocked to prevent governance evasion.\n"
                                        "  Help: Remove '%s' from your command.\n",
                                        flag.c_str(), flag.c_str());
                                    naab::governance::g_governance_hard_block = true;
                                }
                            };
                            implicitBlock("--no-governance", no_governance);
                            implicitBlock("--governance-override", governance_override);
                        }
                    }
                    if (naab::governance::g_governance_hard_block) {
                        fflush(stdout); fflush(stderr);
                        _exit(3);
                    }
                }
            }
            // Reset for actual governance loading in VM/tree-walker path
            naab::governance::g_governance_hard_block = false;
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
            if (no_governance) {
                interpreter.disableGovernance();
            } else if (governance_override) {
                interpreter.setGovernanceOverride(true);
            }
            if (require_governance) {
                interpreter.setRequireGovernance(true);
            }
            interpreter.setGovernanceVerbose(governance_verbose);
            interpreter.setGCThreshold(gc_threshold);

            // Phase 4.2: Enable interactive debugger
            if (debug) {
                auto debugger = std::make_shared<naab::debugger::Debugger>();
                debugger->setActive(true);
                interpreter.setDebugger(debugger);

                // Set breakpoint callback with interactive command loop
                // Bug 5: Capture debugger by value (copy shared_ptr) instead of by reference
                debugger->setBreakpointCallback([&interpreter, debugger](
                    const naab::debugger::Breakpoint& bp,
                    const naab::debugger::CallFrame& frame) {

                    fprintf(stderr, "\n[naab-debug] Hit breakpoint #%d at %s\n",
                            bp.id, bp.location.c_str());
                    if (!frame.function_name.empty()) {
                        fprintf(stderr, "  in function: %s\n", frame.function_name.c_str());
                    }

                    // Interactive command loop
                    while (true) {
                        fprintf(stderr, "(naab-debug) ");
                        fflush(stderr);

                        std::string cmd;
                        if (!std::getline(std::cin, cmd)) {
                            // EOF — quit
                            fprintf(stderr, "[naab-debug] EOF, quitting.\n");
                            std::exit(0);
                        }

                        // Trim whitespace
                        size_t start = cmd.find_first_not_of(" \t");
                        if (start == std::string::npos) continue;
                        cmd = cmd.substr(start);

                        if (cmd == "c" || cmd == "continue") {
                            debugger->resume();
                            break;
                        } else if (cmd == "s" || cmd == "step") {
                            debugger->step(naab::debugger::StepMode::INTO);
                            break;
                        } else if (cmd == "n" || cmd == "next") {
                            debugger->step(naab::debugger::StepMode::OVER);
                            break;
                        } else if (cmd == "o" || cmd == "out") {
                            debugger->step(naab::debugger::StepMode::OUT);
                            break;
                        } else if (cmd == "q" || cmd == "quit") {
                            fprintf(stderr, "[naab-debug] Quitting.\n");
                            std::exit(0);
                        } else if (cmd == "v" || cmd == "vars") {
                            auto vars = interpreter.getCurrentScopeVariables();
                            if (vars.empty()) {
                                fprintf(stderr, "  (no variables in scope)\n");
                            } else {
                                for (const auto& [name, val] : vars) {
                                    if (val.isNull()) continue;
                                    // Skip module markers (both types)
                                    if (val.isString()) {
                                        const auto& s = val.asString();
                                        if (s.size() >= 18 && s.substr(0, 18) == "__stdlib_module__:") continue;
                                        if (s.size() >= 10 && s.substr(0, 10) == "__module__:") continue;
                                    }
                                    // Skip functions
                                    if (val.isFunction()) continue;
                                    fprintf(stderr, "  %s = %s\n", name.c_str(), val.toString().c_str());
                                }
                            }
                        } else if (cmd == "bt" || cmd == "backtrace") {
                            auto stack = debugger->getCallStack();
                            if (stack.empty()) {
                                fprintf(stderr, "  (empty call stack)\n");
                            } else {
                                for (int i = static_cast<int>(stack.size()) - 1; i >= 0; --i) {
                                    fprintf(stderr, "  #%d %s at %s\n",
                                            static_cast<int>(stack.size()) - 1 - i,
                                            stack[i].function_name.c_str(),
                                            stack[i].source_location.c_str());
                                }
                            }
                        } else if (cmd.size() > 2 && cmd[0] == 'p' && cmd[1] == ' ') {
                            // p <variable> — print variable value
                            std::string var_name = cmd.substr(2);
                            size_t vs = var_name.find_first_not_of(" \t");
                            if (vs != std::string::npos) var_name = var_name.substr(vs);
                            auto vars = interpreter.getCurrentScopeVariables();
                            auto it = vars.find(var_name);
                            if (it != vars.end() && !it->second.isNull()) {
                                fprintf(stderr, "  %s = %s\n", var_name.c_str(), it->second.toString().c_str());
                            } else {
                                fprintf(stderr, "  Variable '%s' not found in scope\n", var_name.c_str());
                            }
                        } else if (cmd.size() > 2 && cmd[0] == 'b' && cmd[1] == ' ') {
                            // b <file>:<line> — set breakpoint
                            std::string loc = cmd.substr(2);
                            size_t ls = loc.find_first_not_of(" \t");
                            if (ls != std::string::npos) loc = loc.substr(ls);
                            int bp_id = debugger->setBreakpoint(loc);
                            fprintf(stderr, "  Breakpoint #%d set at %s\n", bp_id, loc.c_str());
                        } else if (cmd.size() > 2 && cmd[0] == 'w' && cmd[1] == ' ') {
                            // w <expr> — add watch
                            std::string expr = cmd.substr(2);
                            size_t es = expr.find_first_not_of(" \t");
                            if (es != std::string::npos) expr = expr.substr(es);
                            int w_id = debugger->addWatch(expr);
                            fprintf(stderr, "  Watch #%d added: %s\n", w_id, expr.c_str());
                        } else if (cmd == "h" || cmd == "help") {
                            fprintf(stderr,
                                "  c(ontinue)     Continue to next breakpoint\n"
                                "  s(tep)         Step into\n"
                                "  n(ext)         Step over\n"
                                "  o(ut)          Step out\n"
                                "  p <var>        Print variable\n"
                                "  v(ars)         Show all variables\n"
                                "  bt             Backtrace\n"
                                "  b <file>:<ln>  Set breakpoint\n"
                                "  w <expr>       Add watch\n"
                                "  q(uit)         Quit\n"
                            );
                        } else {
                            fprintf(stderr, "  Unknown command: %s (type 'h' for help)\n", cmd.c_str());
                        }
                    }
                });

                // Bug 13: Push initial frame so step-INTO has a frame for the callback
                naab::debugger::CallFrame main_frame("main", filename + ":1", 0);
                debugger->pushFrame(main_frame);

                // Start in step mode so we break at first statement
                debugger->step(naab::debugger::StepMode::INTO);

                fprintf(stderr, "[naab-debug] Debugger attached. Will break at first statement.\n"
                                "  Commands: c(ontinue) s(tep) n(ext) o(ut) p <var> v(ars) bt q(uit)\n"
                                "  Set breakpoints: b <file>:<line>\n");
            }

            // Parse
            interpreter.profileStart("Parsing");
            naab::parser::Parser parser(tokens);
            parser.setSource(source, filename);  // Phase 3.1: Set source for AST location tracking
            auto program = parser.parseProgram();
            interpreter.profileEnd("Parsing");

            // Type checking pass (advisory by default, strict with --strict-types)
            if (strict_types || verbose) {
                naab::typecheck::TypeChecker type_checker;
                // Non-owning shared_ptr — type checker only reads the AST
                auto program_ptr = std::shared_ptr<naab::ast::Program>(program.get(), [](naab::ast::Program*){});
                auto type_errors = type_checker.check(program_ptr);
                if (!type_errors.empty()) {
                    if (strict_types) {
                        fmt::print(stderr, "Type check failed ({} error{}):\n",
                            type_errors.size(), type_errors.size() == 1 ? "" : "s");
                        for (const auto& err : type_errors) {
                            fmt::print(stderr, "  {}\n", err.toString());
                        }
                        fflush(stderr);
                        _exit(1);
                    } else if (verbose) {
                        fmt::print(stderr, "[typecheck] {} warning{}:\n",
                            type_errors.size(), type_errors.size() == 1 ? "" : "s");
                        for (const auto& err : type_errors) {
                            fmt::print(stderr, "  {}\n", err.toString());
                        }
                    }
                }
            }

            // Phase 3.1: Set source code for enhanced error messages
            // Skip for VM path — VM has its own governance setup
            if (!use_vm) {
                interpreter.setSourceCode(source, filename);
            }

            // CLI governance overrides (after govern.json is loaded)
            // VM mode handles its own governance setup
            if (!use_vm) {
                auto* gov = interpreter.getGovernance();
                if (gov) {
                    // Agent identity and telemetry
                    gov->setAgentId(agent_id);
                    if (!governance_telemetry_path.empty()) {
                        auto& r = gov->getMutableRules();
                        r.telemetry_output.enabled = true;
                        r.telemetry_output.output_file = governance_telemetry_path;
                        r.telemetry_output.tamper_evidence.enabled = true;
                    }
                    gov->applyAgentRole();
                    // Feature 5: Apply environment overrides
                    if (!governance_env.empty()) {
                        gov->applyEnvironment(governance_env);
                    }
                }
            }
            if (!use_vm && (!governance_report_json.empty() || !governance_report_sarif.empty() ||
                !governance_report_junit.empty())) {
                auto* gov = interpreter.getGovernance();
                if (gov) {
                    auto& rules = gov->getMutableRules();
                    if (!governance_report_json.empty())
                        rules.output.file_output.report_json = governance_report_json;
                    if (!governance_report_sarif.empty())
                        rules.output.file_output.report_sarif = governance_report_sarif;
                    if (!governance_report_junit.empty())
                        rules.output.file_output.report_junit = governance_report_junit;
                    if (governance_record_baselines) {
                        rules.baselines.enabled = true;
                        rules.baselines.auto_record = true;
                    }
                    if (governance_check_baselines) {
                        rules.baselines.enabled = true;
                        rules.baselines.level = naab::governance::EnforcementLevel::HARD;
                    }
                }
            }

            // V-SC-004: lock-check is a pre-execution security gate, not a post-run report.
            // Must halt before any user code runs if the lockfile is invalid or runtime has drifted.
            if (lock_check) {
                std::string lf_path_precheck = lock_path;
                if (lf_path_precheck.empty()) {
                    auto script_dir_pc = std::filesystem::absolute(filename).parent_path().string();
                    lf_path_precheck = naab::discoverLockfilePath(script_dir_pc);
                    if (lf_path_precheck.empty()) {
                        lf_path_precheck = std::filesystem::absolute(filename).parent_path().string()
                                          + "/.naab/naab.lock";
                    }
                }
                if (!naab::Lockfile::verifySignature(lf_path_precheck)) {
                    fprintf(stderr, "[lock] TAMPER DETECTED: lockfile signature mismatch.\n"
                                    "  The lockfile may have been modified by an attacker.\n"
                                    "  Re-run with --lock to regenerate: naab-lang --lock %s\n",
                                    filename.c_str());
                    fflush(stderr);
                    _exit(1);
                }
                auto& lang_registry_pc = naab::runtime::LanguageRegistry::instance();
                std::unordered_map<std::string, std::string> observed_pc;
                for (const auto& lang : lang_registry_pc.supportedLanguages()) {
                    auto* exec = lang_registry_pc.getExecutor(lang);
                    if (exec) {
                        std::string ver = exec->getRuntimeVersion();
                        if (!ver.empty()) observed_pc[lang] = ver;
                    }
                }
                naab::Lockfile lf_pc = naab::Lockfile::load(lf_path_precheck);
                auto drifts_pc = lf_pc.checkDrift(observed_pc);
                if (!drifts_pc.empty()) {
                    for (const auto& d : drifts_pc) {
                        fprintf(stderr, "[lock] DRIFT: %s\n", d.c_str());
                    }
                    fprintf(stderr, "[lock] Runtime versions differ from lockfile.\n"
                                    "  Run with --lock to update: naab-lang --lock %s\n",
                                    filename.c_str());
                    fflush(stdout); fflush(stderr);
                    _exit(1);
                } else if (verbose) {
                    fprintf(stderr, "[lock] All runtime versions match lockfile.\n");
                }
            }

            size_t vm_allocation_count = 0;  // S7: capture VM alloc count for --gc-stats
            if (use_vm) {
                // Governance: reuse shared_governance loaded before preflight (no TOCTOU)
                auto& vm_governance = shared_governance;
                auto script_dir = std::filesystem::absolute(filename).parent_path();
                // Integrity: if signature verification failed, hard block immediately
                if (!gov_loaded && naab::governance::g_governance_hard_block) {
                    fprintf(stderr,
                        "[governance] INTEGRITY BLOCK: Governance configuration is tamper-protected and\n"
                        "  cannot be loaded without the correct signing key. Execution is blocked.\n"
                        "  This is by design — do not attempt to modify, delete, or recreate govern.json\n"
                        "  or its .sig file. Only the project owner can authorize changes.\n");
                    fflush(stderr);
                    _exit(3);
                }
                if (!gov_loaded && !no_governance) {
                    // F2: Detect govern.json deletion — governance artifacts imply prior config
                    {
                        bool had_governance = false;
                        std::error_code f2_ec;
                        auto check_dir = script_dir;
                        for (int depth = 0; depth < 10 && !check_dir.empty(); ++depth) {
                            if (std::filesystem::exists(check_dir / "govern.json.sig", f2_ec) ||
                                std::filesystem::exists(check_dir / ".naab_cache", f2_ec)) {
                                had_governance = true;
                                break;
                            }
                            auto parent = check_dir.parent_path();
                            if (parent == check_dir) break;
                            check_dir = parent;
                        }
                        if (had_governance) {
                            fprintf(stderr,
                                "[governance] INTEGRITY BLOCK: govern.json is missing but governance "
                                "artifacts exist (govern.json.sig or .naab_cache/).\n"
                                "  This project was previously governed. Missing govern.json may "
                                "indicate tampering.\n"
                                "  Restore govern.json or remove governance artifacts to proceed.\n");
                            fflush(stderr);
                            _exit(3);
                        }
                    }
                    if (require_governance) {
                        fmt::print(stderr,
                            "Error: --require-governance set but no govern.json found.\n"
                            "  Search root: {}\n"
                            "  Create a govern.json or remove --require-governance.\n",
                            script_dir.string());
                        fflush(stderr);
                        return 4;
                    }
                    if (!global_quiet) {
                        fprintf(stderr,
                            "[governance] Warning: No govern.json found in '%s' or any parent directory.\n"
                            "  Running WITHOUT governance restrictions.\n"
                            "  To enforce governance, create a govern.json or use --require-governance.\n",
                            script_dir.string().c_str());
                    }
                }
                if (gov_loaded) {
                    // Schema validation warnings (only when governance is active)
                    if (!no_governance) {
                        auto schema_warnings = vm_governance.validateSchema(vm_governance.getLoadedPath());
                        for (const auto& w : schema_warnings) {
                            fprintf(stderr, "%s\n", w.c_str());
                        }
                    }
                    // Apply govern.json behavior settings, then let CLI flags override
                    const auto& rules = vm_governance.getRules();
                    // Check quiet early so it suppresses the "Loaded" message
                    if (rules.quiet_config && !global_quiet) global_quiet = true;
                    auto mode = vm_governance.getMode();
                    std::string mode_str = (mode == naab::governance::GovernanceMode::ENFORCE) ? "enforce"
                                         : (mode == naab::governance::GovernanceMode::AUDIT)   ? "audit"
                                         : "off";
                    if (!global_quiet) {
                        fprintf(stderr, "[governance] Loaded: %s (mode: %s)\n",
                                vm_governance.getLoadedPath().c_str(), mode_str.c_str());
                    }
                    if (rules.verbose && !governance_verbose) governance_verbose = true;
                    if (rules.dashboard && !governance_dashboard) governance_dashboard = true;
                    if (rules.baseline_save && !governance_baseline_save) governance_baseline_save = true;
                    if (rules.allow_override && !governance_override) governance_override = true;
                    if (rules.lint_only_config && !lint_only) lint_only = true;
                    // Category A: remaining governance behavior
                    if (rules.record_baselines && !governance_record_baselines) governance_record_baselines = true;
                    if (rules.check_baselines && !governance_check_baselines) governance_check_baselines = true;
                    // quiet already applied above (before Loaded message)
                    if (rules.no_color_config && !no_color) no_color = true;
                    if (!rules.report_json.empty() && governance_report_json.empty()) governance_report_json = rules.report_json;
                    if (!rules.report_sarif.empty() && governance_report_sarif.empty()) governance_report_sarif = rules.report_sarif;
                    if (!rules.report_junit.empty() && governance_report_junit.empty()) governance_report_junit = rules.report_junit;
                    if (!rules.default_env.empty() && governance_env.empty()) governance_env = rules.default_env;
                    // Category B: runtime limits
                    // govern.json is authoritative for timeout: apply if it specifies a non-default
                    // value, taking the maximum of govern.json and any CLI --timeout override so
                    // that --timeout can extend but not shrink the govern.json limit.
                    if (rules.runtime.timeout != 30) timeout = std::max(timeout, (unsigned int)rules.runtime.timeout);
                    if (rules.runtime.memory_limit > 0 && memory_limit == 512) memory_limit = rules.runtime.memory_limit;
                    if (rules.runtime.gc_threshold != 5000 && gc_threshold == 5000) gc_threshold = rules.runtime.gc_threshold;
                    if (rules.runtime.gc_stats && !gc_stats) gc_stats = true;
                    // Category C: security
                    bool sandbox_level_changed = false;
                    if (!rules.sandbox_level_config.empty() && sandbox_level == "unrestricted") {
                        sandbox_level = rules.sandbox_level_config;
                        sandbox_level_changed = true;
                        // Rebuild security_config from govern.json-specified level
                        if (sandbox_level == "restricted") {
                            security_config = naab::security::SandboxConfig::fromPermissionLevel(
                                naab::security::PermissionLevel::RESTRICTED);
                        } else if (sandbox_level == "standard") {
                            security_config = createEnterpriseConfig();
                        } else if (sandbox_level == "elevated") {
                            security_config = naab::security::SandboxConfig::fromPermissionLevel(
                                naab::security::PermissionLevel::ELEVATED);
                        }
                        security_config.max_cpu_seconds = timeout;
                        security_config.max_memory_mb = memory_limit;
                    }
                    // Fail-closed: enforce mode defaults to standard sandbox (not unrestricted)
                    // Only apply when governance is actually active (not --no-governance)
                    bool upgraded_to_standard = sandbox_level_changed;
                    if (!no_governance &&
                        vm_governance.getMode() == naab::governance::GovernanceMode::ENFORCE &&
                        rules.sandbox_level_config.empty() &&
                        sandbox_level == "unrestricted") {
                        sandbox_level = "standard";
                        security_config = createEnterpriseConfig();
                        security_config.max_cpu_seconds = timeout;
                        security_config.max_memory_mb = memory_limit;
                        upgraded_to_standard = true;
                        fmt::print(stderr,
                            "[governance] Sandbox: upgraded unrestricted → standard "
                            "(enforce mode; set security.sandbox_level in govern.json to override)\n");
                    }
                    // Sync governance capabilities into sandbox (tighten only)
                    // Skip when --no-governance — user explicitly opted out
                    if (!no_governance) {
                        syncGovernanceToSandbox(rules, security_config);
                        naab::security::SandboxManager::instance().setDefaultConfig(security_config);
                        // Update the live sandbox with governance-driven restrictions
                        auto* live_sb = naab::security::ScopedSandbox::getCurrent();
                        if (live_sb) {
                            // When governance changes sandbox level, replace the live
                            // sandbox config with the governance-specified one
                            if (upgraded_to_standard) {
                                live_sb->replaceConfig(security_config);
                            }
                            if (!rules.network_allowed) {
                                live_sb->setNetworkEnabled(false);
                                live_sb->removeCapability(naab::security::Capability::NET_CONNECT);
                            }
                            if (!rules.shell_allowed) {
                                live_sb->setAllowExec(false);
                                live_sb->removeCapability(naab::security::Capability::SYS_EXEC);
                            }
                            if (!rules.capabilities.env_vars.read) {
                                live_sb->removeCapability(naab::security::Capability::SYS_ENV);
                            }
                        }
                    }
                    // Only let governance override sandbox network settings when mode != off.
                    // network_allowed defaults to true, so mode=off (no capabilities section)
                    // would otherwise always enable network — defeating --sandbox-level standard.
                    if (mode != naab::governance::GovernanceMode::OFF &&
                        (rules.allow_network_config || rules.network_allowed)) {
                        network_enabled = true;
                        // Update the active sandbox — it was created before governance loaded
                        // Must set BOTH the bool AND the NET_CONNECT capability;
                        // canConnect() checks both conditions.
                        auto* current_sandbox = naab::security::ScopedSandbox::getCurrent();
                        if (current_sandbox) {
                            current_sandbox->setNetworkEnabled(true);
                            current_sandbox->addCapability(naab::security::Capability::NET_CONNECT);
                        }
                        security_config.network_enabled = true;
                        security_config.addCapability(naab::security::Capability::NET_CONNECT);
                        naab::security::SandboxManager::instance().setDefaultConfig(security_config);
                    }
                    if (rules.strict_types_config && !strict_types) strict_types = true;
                    if (governance_override) vm_governance.setOverrideEnabled(true);
                    if (!override_reason.empty()) vm_governance.setOverrideReason(override_reason);
                    // Agent identity and telemetry
                    if (!rules.agent_id_config.empty() && agent_id == "anonymous") agent_id = rules.agent_id_config;
                    vm_governance.setAgentId(agent_id);
                    if (!rules.telemetry_path.empty() && governance_telemetry_path.empty()) governance_telemetry_path = rules.telemetry_path;
                    if (!governance_telemetry_path.empty()) {
                        auto& rules = vm_governance.getMutableRules();
                        rules.telemetry_output.enabled = true;
                        rules.telemetry_output.output_file = governance_telemetry_path;
                        rules.telemetry_output.tamper_evidence.enabled = true;
                    }
                    vm_governance.applyAgentRole();
                    // Feature 5: Apply environment overrides
                    if (!governance_env.empty()) {
                        vm_governance.applyEnvironment(governance_env);
                    }
                    // Apply CLI report path overrides
                    if (!governance_report_json.empty() || !governance_report_sarif.empty() ||
                        !governance_report_junit.empty()) {
                        auto& rules = vm_governance.getMutableRules();
                        if (!governance_report_json.empty())
                            rules.output.file_output.report_json = governance_report_json;
                        if (!governance_report_sarif.empty())
                            rules.output.file_output.report_sarif = governance_report_sarif;
                        if (!governance_report_junit.empty())
                            rules.output.file_output.report_junit = governance_report_junit;
                    }
                }

                // Integrity: check for blocked CLI flags
                // C4 fix: reuse pre-flight _has_authority (Ed25519/HMAC verified)
                // instead of bare env var presence check
                if (gov_loaded && vm_governance.isActive() && !_has_authority) {
                    // Gap 4/13/16: Implicit blocks in enforce mode (mirrors pre-flight check)
                    auto checkBlocked = [&](const std::string& flag, bool was_used) {
                        if (was_used && vm_governance.isBlockedFlag(flag)) {
                            fprintf(stderr,
                                "[governance] INTEGRITY BLOCK: flag '%s' is locked by the project owner.\n"
                                "  This flag is listed in integrity.blocked_flags in govern.json.\n"
                                "  Help: Remove '%s' from your command to proceed.\n"
                                "  The project owner has restricted this flag to prevent governance bypass.\n"
                                "  To modify governance settings, contact the project owner.\n",
                                flag.c_str(), flag.c_str());
                            naab::governance::g_governance_hard_block = true;
                        }
                    };
                    checkBlocked("--drift-baseline-save", drift_baseline_save);
                    checkBlocked("--governance-override", governance_override);
                    checkBlocked("--governance-baseline-save", governance_baseline_save);
                    checkBlocked("--no-governance", no_governance);

                    // Gap 13/16: In enforce mode with a signed govern.json,
                    // --no-governance and --governance-override are implicitly blocked
                    // even without blocked_flags. Only applies when govern.json.sig exists.
                    if (vm_governance.getMode() == naab::governance::GovernanceMode::ENFORCE) {
                        std::string gov_dir = vm_governance.getGovernDir();
                        bool config_is_signed = !gov_dir.empty() &&
                            std::filesystem::exists(gov_dir + "/govern.json.sig");
                        if (config_is_signed) {
                            auto implicitBlock = [&](const std::string& flag, bool was_used) {
                                if (was_used && !vm_governance.isBlockedFlag(flag)) {
                                    fprintf(stderr,
                                        "[governance] INTEGRITY BLOCK: flag '%s' is not allowed.\n"
                                        "  This project's govern.json is signed — bypass flags are\n"
                                        "  implicitly blocked to prevent governance evasion.\n"
                                        "  Help: Remove '%s' from your command.\n",
                                        flag.c_str(), flag.c_str());
                                    naab::governance::g_governance_hard_block = true;
                                }
                            };
                            implicitBlock("--no-governance", no_governance);
                            implicitBlock("--governance-override", governance_override);
                        }
                    }

                    if (naab::governance::g_governance_hard_block) {
                        if (vm_governance.isActive()) vm_governance.writeReports();
                        fflush(stdout); fflush(stderr);
                        _exit(3);
                    }
                }

                // VM path: check NAAb source for secrets/PII/incomplete logic
                // (tree-walker does this per-function in checkNaabFunctionBody)
                if (gov_loaded && vm_governance.isActive()) {
                    std::string sec_err = vm_governance.checkSecrets(source, 0);
                    if (!sec_err.empty()) throw std::runtime_error(sec_err);
                    sec_err = vm_governance.checkPii(source, 0);
                    if (!sec_err.empty()) throw std::runtime_error(sec_err);
                    sec_err = vm_governance.checkIncompleteLogic(source, 0, filename);
                    if (!sec_err.empty()) throw std::runtime_error(sec_err);
                }

                // Gap 15: Detect empty/comment-only main{} blocks
                if (gov_loaded && vm_governance.isActive() && program->getMainBlock()) {
                    std::string empty_err = vm_governance.checkEmptyMain(source);
                    if (!empty_err.empty()) {
                        fprintf(stderr, "%s\n", empty_err.c_str());
                    }
                }

                // Preflight intent gate — verify all functions match owner-defined intent
                // before any compilation or execution. Pattern-based (fast, no API).
                if (gov_loaded && vm_governance.isActive()) {
                    std::string intent_err = vm_governance.preflightIntentCheck(*program, source);
                    if (!intent_err.empty()) {
                        throw std::runtime_error(intent_err);
                    }
                }

                // Drift detection: check structural metrics against baseline
                if (gov_loaded && vm_governance.getRules().code_quality.drift_detection.enabled) {
                    auto drift_metrics = naab::governance::GovernanceEngine::collectDriftMetrics(
                        *program, source, filename);
                    std::string drift_err = vm_governance.checkDriftDetection(filename, drift_metrics);
                    if (!drift_err.empty()) {
                        fprintf(stderr, "%s", drift_err.c_str());
                    }
                    if (drift_baseline_save || vm_governance.getRules().code_quality.drift_detection.auto_save) {
                        vm_governance.saveDriftBaseline(filename, drift_metrics);
                    }
                } else if (drift_baseline_save && gov_loaded) {
                    // Save baseline even if drift_detection not enabled in config
                    auto drift_metrics = naab::governance::GovernanceEngine::collectDriftMetrics(
                        *program, source, filename);
                    vm_governance.saveDriftBaseline(filename, drift_metrics);
                }

                // Preflight scanner: run before execution so advisories are visible
                // even if governance blocks execution later (exit 3)
                if (gov_loaded && vm_governance.isActive()) {
                    naab::scanner::ScannerEngine preflight_scanner;
                    preflight_scanner.loadConfigFromPath(vm_governance.getLoadedPath(), true);
                    if (preflight_scanner.hasConfig()) {
                        // Collect all files to scan: main file + imported modules
                        std::vector<std::string> scan_files = {filename};
                        std::filesystem::path main_dir = std::filesystem::path(filename).parent_path();
                        for (const auto& imp : program->getModuleImports()) {
                            const std::string& mp = imp->getModulePath();
                            // Skip stdlib imports (bare names without .naab extension)
                            if (mp.find('/') == std::string::npos &&
                                mp.find('\\') == std::string::npos &&
                                (mp.size() < 5 || mp.substr(mp.size() - 5) != ".naab")) {
                                continue;
                            }
                            auto resolved = main_dir / mp;
                            if (std::filesystem::exists(resolved)) {
                                scan_files.push_back(resolved.string());
                            }
                        }

                        int total_adv_count = 0;
                        std::vector<std::string> adv_lines;
                        for (const auto& scan_file : scan_files) {
                            auto scan_result = preflight_scanner.scan(scan_file, "auto");
                            for (const auto& issue : scan_result.issues) {
                                if (issue.level == "advisory") {
                                    total_adv_count++;
                                    if (adv_lines.size() < 10) {
                                        std::string line_str = fmt::format("  - Line {}: {}.{} — {}",
                                            issue.line, issue.category, issue.rule, issue.message);
                                        if (scan_file != filename) {
                                            // Show relative path for imported files
                                            line_str = fmt::format("  - {} Line {}: {}.{} — {}",
                                                std::filesystem::path(scan_file).filename().string(),
                                                issue.line, issue.category, issue.rule, issue.message);
                                        }
                                        if (!issue.fix.empty()) {
                                            line_str += fmt::format("\n    Fix: {}", issue.fix);
                                        }
                                        adv_lines.push_back(line_str);
                                    }
                                }
                            }
                        }
                        if (total_adv_count > 0) {
                            fprintf(stderr, "\n[scanner] Pre-flight notes (%d advisory):\n", total_adv_count);
                            for (const auto& line : adv_lines) {
                                fprintf(stderr, "%s\n", line.c_str());
                            }
                            if (total_adv_count > 10) {
                                fprintf(stderr, "  ... and %d more\n", total_adv_count - 10);
                            }
                            fprintf(stderr, "\n");
                        }
                    }
                }

                // Store source for governance voice synthesis
                if (gov_loaded && vm_governance.isActive()) {
                    vm_governance.setSource(source);
                }

                // Agent review: LLM-based governance phase (if enabled in govern.json)
                if (gov_loaded && vm_governance.isActive()) {
                    vm_governance.runAgentReview(source);
                    // Intent-specific agent findings block execution immediately
                    if (vm_governance.hasIntentBlock()) {
                        vm_governance.writeReports();
                        fflush(stdout); fflush(stderr);
                        _exit(2);  // quality gate failure
                    }
                }

                // Drift detection / agent review hard block — skip execution
                if (naab::governance::g_governance_hard_block) {
                    if (vm_governance.isActive()) {
                        vm_governance.runGovernanceVoice();
                        vm_governance.writeReports();
                    }
                    fflush(stdout); fflush(stderr);
                    _exit(3);
                }

                // Bytecode VM path — compiler gets governance for pre-flight taint analysis
                naab::vm::Compiler bc_compiler;
                if (gov_loaded) bc_compiler.setGovernance(&vm_governance);
                auto* main_fn = bc_compiler.compile(*program, filename);
                if (!main_fn) {
                    throw std::runtime_error("Compilation failed: " + bc_compiler.getLastError());
                }
                if (verbose) {
                    naab::vm::disassembleChunk(main_fn->chunk, filename);
                }
                naab::vm::VM bytecode_vm;
                naab::stdlib::StdLib vm_stdlib;
                naab::modules::ModuleResolver vm_module_resolver;
                bytecode_vm.setStdlib(&vm_stdlib);
                bytecode_vm.setModuleResolver(&vm_module_resolver);
                bytecode_vm.setCurrentFile(filename);

                // ISS-028: Pass CLI args to VM env module
                bytecode_vm.setScriptArgs(script_args);
                auto env_module = vm_stdlib.getModule("env");
                if (env_module) {
                    auto* env_mod = dynamic_cast<naab::stdlib::EnvModule*>(env_module.get());
                    if (env_mod) {
                        env_mod->setArgsProvider(
                            [&script_args]() -> std::vector<std::string> {
                                return script_args;
                            }
                        );
                    }
                }

                if (gov_loaded) {
                    if (governance_verbose) bytecode_vm.setGovernanceVerbose(true);
                    bytecode_vm.setGovernance(&vm_governance);
                    naab::governance::GovernanceEngine::setCurrent(&vm_governance);
                }
                bytecode_vm.setGCThreshold(gc_threshold);  // V-RT-008

                // Profiler
                bytecode_vm.setProfileMode(profile);
                if (profile) {
                    naab::profiling::Profiler::instance().enable();
                }

                // Debugger
                std::shared_ptr<naab::debugger::Debugger> vm_debugger;
                if (debug) {
                    vm_debugger = std::make_shared<naab::debugger::Debugger>();
                    vm_debugger->setActive(true);
                    bytecode_vm.setDebugger(vm_debugger.get());

                    // Interactive command loop (same as tree-walker)
                    vm_debugger->setBreakpointCallback([&bytecode_vm, vm_debugger](
                        const naab::debugger::Breakpoint& bp,
                        const naab::debugger::CallFrame& dbg_frame) {

                        fprintf(stderr, "\n[naab-debug] Hit breakpoint #%d at %s\n",
                                bp.id, bp.location.c_str());
                        if (!dbg_frame.function_name.empty()) {
                            fprintf(stderr, "  in function: %s\n", dbg_frame.function_name.c_str());
                        }

                        while (true) {
                            fprintf(stderr, "(naab-debug) ");
                            fflush(stderr);

                            std::string cmd;
                            if (!std::getline(std::cin, cmd)) {
                                fprintf(stderr, "[naab-debug] EOF, quitting.\n");
                                std::exit(0);
                            }

                            size_t start = cmd.find_first_not_of(" \t");
                            if (start == std::string::npos) continue;
                            cmd = cmd.substr(start);

                            if (cmd == "c" || cmd == "continue") {
                                vm_debugger->resume();
                                break;
                            } else if (cmd == "s" || cmd == "step") {
                                vm_debugger->step(naab::debugger::StepMode::INTO);
                                break;
                            } else if (cmd == "n" || cmd == "next") {
                                vm_debugger->step(naab::debugger::StepMode::OVER);
                                break;
                            } else if (cmd == "o" || cmd == "out") {
                                vm_debugger->step(naab::debugger::StepMode::OUT);
                                break;
                            } else if (cmd == "q" || cmd == "quit") {
                                fprintf(stderr, "[naab-debug] Quitting.\n");
                                std::exit(0);
                            } else if (cmd == "v" || cmd == "vars") {
                                auto vars = bytecode_vm.getCurrentScopeVariables();
                                if (vars.empty()) {
                                    fprintf(stderr, "  (no variables in scope)\n");
                                } else {
                                    for (const auto& [name, val] : vars) {
                                        if (val.isNull()) continue;
                                        if (val.isString()) {
                                            const auto& s = val.asString();
                                            if (s.size() >= 18 && s.substr(0, 18) == "__stdlib_module__:") continue;
                                            if (s.size() >= 10 && s.substr(0, 10) == "__module__:") continue;
                                        }
                                        if (val.isFunction() || val.isVMClosure()) continue;
                                        fprintf(stderr, "  %s = %s\n", name.c_str(), val.toString().c_str());
                                    }
                                }
                            } else if (cmd == "bt" || cmd == "backtrace") {
                                auto stack = vm_debugger->getCallStack();
                                if (stack.empty()) {
                                    fprintf(stderr, "  (empty call stack)\n");
                                } else {
                                    for (int i = static_cast<int>(stack.size()) - 1; i >= 0; --i) {
                                        fprintf(stderr, "  #%d %s at %s\n",
                                                static_cast<int>(stack.size()) - 1 - i,
                                                stack[i].function_name.c_str(),
                                                stack[i].source_location.c_str());
                                    }
                                }
                            } else if (cmd.size() > 2 && cmd[0] == 'p' && cmd[1] == ' ') {
                                std::string var_name = cmd.substr(2);
                                size_t vs = var_name.find_first_not_of(" \t");
                                if (vs != std::string::npos) var_name = var_name.substr(vs);
                                auto vars = bytecode_vm.getCurrentScopeVariables();
                                auto it = vars.find(var_name);
                                if (it != vars.end() && !it->second.isNull()) {
                                    fprintf(stderr, "  %s = %s\n", var_name.c_str(), it->second.toString().c_str());
                                } else {
                                    fprintf(stderr, "  Variable '%s' not found in scope\n", var_name.c_str());
                                }
                            } else if (cmd.size() > 2 && cmd[0] == 'b' && cmd[1] == ' ') {
                                std::string loc = cmd.substr(2);
                                size_t ls = loc.find_first_not_of(" \t");
                                if (ls != std::string::npos) loc = loc.substr(ls);
                                int bp_id = vm_debugger->setBreakpoint(loc);
                                fprintf(stderr, "  Breakpoint #%d set at %s\n", bp_id, loc.c_str());
                            } else if (cmd.size() > 2 && cmd[0] == 'w' && cmd[1] == ' ') {
                                std::string expr = cmd.substr(2);
                                size_t es = expr.find_first_not_of(" \t");
                                if (es != std::string::npos) expr = expr.substr(es);
                                int w_id = vm_debugger->addWatch(expr);
                                fprintf(stderr, "  Watch #%d added: %s\n", w_id, expr.c_str());
                            } else if (cmd == "h" || cmd == "help") {
                                fprintf(stderr,
                                    "  c(ontinue)     Continue to next breakpoint\n"
                                    "  s(tep)         Step into\n"
                                    "  n(ext)         Step over\n"
                                    "  o(ut)          Step out\n"
                                    "  p <var>        Print variable\n"
                                    "  v(ars)         Show all variables\n"
                                    "  bt             Backtrace\n"
                                    "  b <file>:<ln>  Set breakpoint\n"
                                    "  w <expr>       Add watch\n"
                                    "  q(uit)         Quit\n"
                                );
                            } else {
                                fprintf(stderr, "  Unknown command: %s (type 'h' for help)\n", cmd.c_str());
                            }
                        }
                    });

                    // Push initial main frame and start in step mode
                    naab::debugger::CallFrame main_frame("main", filename + ":1", 0);
                    vm_debugger->pushFrame(main_frame);
                    vm_debugger->step(naab::debugger::StepMode::INTO);

                    fprintf(stderr, "[naab-debug] Debugger attached (VM mode). Will break at first statement.\n"
                                    "  Commands: c(ontinue) s(tep) n(ext) o(ut) p <var> v(ars) bt q(uit)\n"
                                    "  Set breakpoints: b <file>:<line>\n");
                }

                // VM block imports: load blocks from UseStatements and inject into globals
                for (auto& import : program->getImports()) {
                    auto& block_registry = naab::runtime::BlockRegistry::instance();
                    if (!block_registry.isInitialized()) {
                        std::string home_dir = std::getenv("HOME") ? std::getenv("HOME") : ".";
                        std::string blocks_path = home_dir + "/.naab/language/blocks/library/";
                        block_registry.initialize(blocks_path);
                    }
                    auto metadata_opt = block_registry.getBlock(import->getBlockId());
                    if (metadata_opt.has_value()) {
                        auto metadata = *metadata_opt;
                        auto code = block_registry.getBlockSource(import->getBlockId());
                        auto& lang_registry = naab::runtime::LanguageRegistry::instance();
                        auto* executor = lang_registry.getExecutor(metadata.language);
                        if (executor) {
                            // Execute block code to define functions in executor context
#ifdef HAVE_QUICKJS
                            if (metadata.language == "javascript") {
                                auto* js_exec = dynamic_cast<naab::runtime::JsExecutorAdapter*>(executor);
                                if (js_exec) js_exec->execute(code, naab::runtime::JsExecutionMode::BLOCK_LIBRARY);
                            } else
#endif
#ifndef _WIN32
                            if (metadata.language == "cpp" || metadata.language == "c++") {
                                auto* cpp_exec = dynamic_cast<naab::runtime::CppExecutorAdapter*>(executor);
                                if (cpp_exec) cpp_exec->execute(code, naab::runtime::CppExecutionMode::BLOCK_LIBRARY);
                            } else
#endif
                            {
                                executor->execute(code);
                            }
                            auto block_value = std::make_shared<naab::interpreter::BlockValue>(
                                metadata, code, executor);
#ifndef _WIN32
                            // Store C++ block ID for multi-block executor sharing
                            if (metadata.language == "cpp" || metadata.language == "c++") {
                                auto* cpp_exec = dynamic_cast<naab::runtime::CppExecutorAdapter*>(executor);
                                if (cpp_exec) block_value->cpp_block_id = cpp_exec->getCurrentBlockId();
                            }
#endif
                            bytecode_vm.setGlobal(import->getAlias(),
                                naab::interpreter::NaabVal::makeBlock(block_value));
                        }
                    }
                }

                // V-LSP-005: lint-only — compile + pre-flight governance; skip execution.
                if (lint_only) {
                    if (vm_governance.isActive()) {
                        vm_governance.writeReports();
                        std::string gate = vm_governance.evaluateQualityGate();
                        if (!gate.empty()) {
                            fprintf(stderr, "%s", gate.c_str());
                            fflush(stdout); fflush(stderr);
                            _exit(2);
                        }
                    }
                    fflush(stdout); fflush(stderr);
                    _exit(naab::governance::g_governance_hard_block ? 3 : 0);
                }

                // Finding A fix: arm SIGALRM for NAAb VM execution itself.
                // Previously timeout only guarded polyglot subprocesses (security_config).
                // ScopedTimeout disarms the alarm automatically on any exit path.
                naab::security::ScopedTimeout vm_timeout(timeout);

                // Set VM callbacks for tool execution during agent.send()
                // (must be set before execute so callVMFunction works in tool loops)
                if (vm_governance.isActive()) {
                    vm_governance.setVMCallbacks(
                        [&bytecode_vm](naab::interpreter::NaabVal fn, const std::vector<naab::interpreter::NaabVal>& args, bool taint) {
                            return bytecode_vm.callNaabFunction(fn, args, taint);
                        },
                        [&bytecode_vm]() -> const auto& {
                            return bytecode_vm.getGlobals();
                        }
                    );
                }

                try {
                    auto result = bytecode_vm.execute(main_fn);
                    (void)result;
                    // writeReports() already called inside execute() on success
                } catch (...) {
                    if (vm_governance.isActive()) {
                        vm_governance.runGovernanceVoice();
                        vm_governance.writeReports();
                    }
                    throw;
                }

                // Print profile report
                if (profile) {
                    auto report = naab::profiling::Profiler::instance().generateReport();
                    fmt::print(stderr, "{}", report.toString());
                }

                // Pass 2: Post-execution validation audit (VM path)
                if (vm_governance.isActive() && !no_governance) {
                    vm_governance.runPostExecutionAudit();
                }

                // Pass 3: Execution-based contract tests (must_produce, must_vary, etc.)
                if (vm_governance.isActive() && !no_governance) {
                    vm_governance.setVMCallbacks(
                        [&bytecode_vm](naab::interpreter::NaabVal fn, const std::vector<naab::interpreter::NaabVal>& args, bool taint) {
                            return bytecode_vm.callNaabFunction(fn, args, taint);
                        },
                        [&bytecode_vm]() -> const auto& {
                            return bytecode_vm.getGlobals();
                        }
                    );
                    std::string contract_err = vm_governance.runExecutionContracts();
                    vm_governance.clearVMCallbacks();
                    if (!contract_err.empty()) {
                        fprintf(stderr, "Error: %s\n", contract_err.c_str());
                        vm_governance.writeReports();
                        fflush(stdout); fflush(stderr);
                        _exit(3);
                    }
                }

                // Print governance dashboard summary
                if (governance_dashboard && vm_governance.isActive()) {
                    vm_governance.printDashboard();
                }

                // Feature 2: Quality gate check (VM path)
                if (vm_governance.isActive()) {
                    std::string gate = vm_governance.evaluateQualityGate();
                    if (!gate.empty()) {
                        fprintf(stderr, "%s", gate.c_str());
                        vm_governance.writeReports();
                        fflush(stdout); fflush(stderr);
                        _exit(2);
                    }
                }

                // Feature 4: Baseline regression check (VM path)
                if (vm_governance.isActive()) {
                    std::string regression = vm_governance.checkGovernanceBaseline();
                    if (!regression.empty()) {
                        fprintf(stderr, "%s", regression.c_str());
                        if (vm_governance.getRules().governance_baseline.fail_on_regression) {
                            vm_governance.writeReports();
                            fflush(stdout); fflush(stderr);
                            _exit(2);
                        }
                    }
                    if (governance_baseline_save) {
                        vm_governance.saveGovernanceBaseline();
                    }
                }
                vm_allocation_count = bytecode_vm.getAllocationCount();

                // Clear dangling governance pointer before vm_governance goes out of scope
                naab::governance::GovernanceEngine::setCurrent(nullptr);
            } else {
                // V-LSP-005: lint-only gate for tree-walker path.
                if (lint_only) {
                    auto* gov = interpreter.getGovernance();
                    if (gov) {
                        gov->writeReports();
                        std::string gate = gov->evaluateQualityGate();
                        if (!gate.empty()) {
                            fprintf(stderr, "%s", gate.c_str());
                            fflush(stdout); fflush(stderr);
                            _exit(2);
                        }
                    }
                    fflush(stdout); fflush(stderr);
                    _exit(naab::governance::g_governance_hard_block ? 3 : 0);
                }

                // Preflight intent gate (tree-walker path)
                // F5: Use shared_governance (loaded at startup) — interpreter's engine
                // hasn't called discoverAndLoad() yet (happens inside execute()).
                if (gov_loaded && shared_governance.isActive()) {
                    std::string intent_err = shared_governance.preflightIntentCheck(*program, source);
                    if (!intent_err.empty()) {
                        throw std::runtime_error(intent_err);
                    }
                }

                // Drift detection (tree-walker path — pre-execute)
                {
                    auto* gov = interpreter.getGovernance();
                    if (gov && gov->getRules().code_quality.drift_detection.enabled) {
                        auto drift_metrics = naab::governance::GovernanceEngine::collectDriftMetrics(
                            *program, source, filename);
                        std::string drift_err = gov->checkDriftDetection(filename, drift_metrics);
                        if (!drift_err.empty()) {
                            fprintf(stderr, "%s", drift_err.c_str());
                        }
                        if (drift_baseline_save || gov->getRules().code_quality.drift_detection.auto_save) {
                            gov->saveDriftBaseline(filename, drift_metrics);
                        }
                    } else if (drift_baseline_save) {
                        auto* gov2 = interpreter.getGovernance();
                        if (gov2) {
                            auto drift_metrics = naab::governance::GovernanceEngine::collectDriftMetrics(
                                *program, source, filename);
                            gov2->saveDriftBaseline(filename, drift_metrics);
                        }
                    }
                }

                // Store source for governance voice synthesis (tree-walker path)
                if (gov_loaded && shared_governance.isActive()) {
                    shared_governance.setSource(source);
                }

                // Agent review: LLM-based governance phase (tree-walker path)
                // F5: Use shared_governance — same rationale as preflight gate above
                if (gov_loaded && shared_governance.isActive()) {
                    shared_governance.runAgentReview(source);
                    if (shared_governance.hasIntentBlock()) {
                        shared_governance.writeReports();
                        fflush(stdout); fflush(stderr);
                        _exit(2);
                    }
                }

                // Drift detection / agent review hard block — skip execution
                if (naab::governance::g_governance_hard_block) {
                    auto* gov = interpreter.getGovernance();
                    if (gov) {
                        gov->runGovernanceVoice();
                        gov->writeReports();
                    }
                    fflush(stdout); fflush(stderr);
                    _exit(3);
                }

                // Inner try-catch: write governance reports before re-throwing.
                // The interpreter is alive here, so we can safely access it.
                // Finding A fix: arm SIGALRM for tree-walker execution (mirrors VM path above).
                naab::security::ScopedTimeout tw_timeout(timeout);

                // On success, writeReports() is already called inside execute().
                try {
                    interpreter.execute(*program);
                } catch (...) {
                    // Write reports before the interpreter is destroyed
                    auto* gov = interpreter.getGovernance();
                    if (gov) {
                        gov->runGovernanceVoice();
                        gov->writeReports();
                    }
                    throw;  // Re-throw to outer catch
                }

                // Apply govern.json behavior settings for tree-walker
                // (governance loads during execute(), so settings available now)
                {
                    auto* gov = interpreter.getGovernance();
                    if (gov && gov->isActive()) {
                        const auto& r = gov->getRules();
                        if (r.verbose && !governance_verbose) governance_verbose = true;
                        if (r.dashboard && !governance_dashboard) governance_dashboard = true;
                        if (r.baseline_save && !governance_baseline_save) governance_baseline_save = true;
                    }
                }

                // Pass 2: Post-execution validation audit (tree-walker path)
                {
                    auto* gov = interpreter.getGovernance();
                    if (gov && gov->isActive()) gov->runPostExecutionAudit();
                }

                // Pass 3: Execution-based contract tests (must_produce, must_vary, etc.)
                {
                    auto* gov = interpreter.getGovernance();
                    if (gov && gov->isActive() && !no_governance) {
                        std::string contract_err = gov->runExecutionContracts();
                        if (!contract_err.empty()) {
                            fprintf(stderr, "Error: %s\n", contract_err.c_str());
                            gov->writeReports();
                            fflush(stdout); fflush(stderr);
                            _exit(3);
                        }
                    }
                }

                // Print governance dashboard summary
                if (governance_dashboard) {
                    auto* gov = interpreter.getGovernance();
                    if (gov) gov->printDashboard();
                }

                // Feature 2: Quality gate check (tree-walker path)
                {
                    auto* gov = interpreter.getGovernance();
                    if (gov) {
                        std::string gate = gov->evaluateQualityGate();
                        if (!gate.empty()) {
                            fprintf(stderr, "%s", gate.c_str());
                            gov->writeReports();
                            fflush(stdout); fflush(stderr);
                            _exit(2);
                        }
                    }
                }

                // Feature 4: Baseline regression check (tree-walker path)
                {
                    auto* gov = interpreter.getGovernance();
                    if (gov) {
                        std::string regression = gov->checkGovernanceBaseline();
                        if (!regression.empty()) {
                            fprintf(stderr, "%s", regression.c_str());
                            if (gov->getRules().governance_baseline.fail_on_regression) {
                                gov->writeReports();
                                fflush(stdout); fflush(stderr);
                                _exit(2);
                            }
                        }
                        if (governance_baseline_save) {
                            gov->saveGovernanceBaseline();
                        }
                    }
                }

            }

            if (gc_stats) {
                size_t alloc_count = use_vm ? vm_allocation_count : interpreter.getAllocationCount();
                fmt::print("[GC] Allocations tracked: {}, Collections run: {}\n",
                           alloc_count,
                           interpreter.getGCCollectionCount());
            }

            if (profile) {
                interpreter.printProfile();
            }

            // Phase 8.4: Lockfile support — record observed runtime versions post-execution.
            // NOTE: lock_check (V-SC-004) was moved to a pre-execution gate above.
            if (lock_update) {
                // Discover lockfile path if not explicitly provided
                std::string lf_path = lock_path;
                if (lf_path.empty()) {
                    auto script_dir = std::filesystem::absolute(filename).parent_path().string();
                    lf_path = naab::discoverLockfilePath(script_dir);
                    if (lf_path.empty()) {
                        // Default: .naab/naab.lock next to the script
                        lf_path = std::filesystem::absolute(filename).parent_path().string()
                                  + "/.naab/naab.lock";
                    }
                }

                // Collect observed runtime versions from all registered executors
                auto& lang_registry = naab::runtime::LanguageRegistry::instance();
                std::unordered_map<std::string, std::string> observed;
                for (const auto& lang : lang_registry.supportedLanguages()) {
                    auto* exec = lang_registry.getExecutor(lang);
                    if (exec) {
                        std::string ver = exec->getRuntimeVersion();
                        if (!ver.empty()) observed[lang] = ver;
                    }
                }

                naab::Lockfile lf = naab::Lockfile::load(lf_path);
                for (const auto& [lang, ver] : observed) {
                    lf.update(lang, ver);
                }
                lf.save(lf_path);
                if (verbose) {
                    fprintf(stderr, "[lock] Lockfile updated: %s\n", lf_path.c_str());
                }
            }

            // Use _exit() after interpreter runs - thread pool workers and
            // Python thread states trigger bionic CFI crashes during static
            // destruction on Android. _exit() is safe: the OS cleans up all
            // process resources, and we've already flushed all output.
            fflush(stdout);
            fflush(stderr);
            _exit(0);

        } catch (const naab::governance::GovernanceHardError& e) {
            // Uncatchable governance violation — NAAb try/catch cannot swallow this
            std::string msg = naab::error::ErrorSanitizer::sanitize(e.what());
            fmt::print("Error: {}\n", msg);
            fflush(stdout);
            fflush(stderr);
            _exit(3);
        } catch (const naab::limits::ExitException& e) {
            // V-DOS-014: process.exit() throws ExitException instead of std::exit()
            fflush(stdout);
            fflush(stderr);
            // naab-29 L-02: governance hard block overrides process.exit()
            if (naab::governance::g_governance_hard_block) _exit(3);
            _exit(e.exit_code);
        } catch (const naab::interpreter::NaabError& e) {
            // NaabError has full stack trace - print it
            // V-ERR-002: sanitize before displaying to prevent sensitive data leakage
            fmt::print("{}\n", naab::error::ErrorSanitizer::sanitize(e.formatError()));
            fflush(stdout);
            fflush(stderr);
            if (naab::governance::g_governance_hard_block) _exit(3);
            _exit(1);
        } catch (const std::exception& e) {
            // V-ERR-002: keep raw_msg for exit-code detection; display sanitized version
            std::string raw_msg = e.what();
            std::string msg = naab::error::ErrorSanitizer::sanitize(raw_msg);
            fmt::print("Error: {}\n", msg);
            fflush(stdout);
            fflush(stderr);
            if (naab::governance::g_governance_hard_block) _exit(3);
            if (raw_msg.find("Governance config error:") == 0) _exit(4);
            _exit(1);
        }

    } else if (command == "parse") {
        if (argc < 3) {
            fmt::print("Usage: naab-lang parse <file.naab>\n");
            fmt::print("  Parse a .naab file and display AST summary.\n");
            return 0;
        }
        std::string filename = argv[2];
        if (filename == "--help" || filename == "-h") {
            fmt::print("Usage: naab-lang parse <file.naab>\n");
            fmt::print("  Parse a .naab file and display AST summary.\n");
            return 0;
        }

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
            fmt::print("Usage: naab-lang check <file.naab>\n");
            fmt::print("  Type-check a .naab file without executing it.\n");
            return 0;
        }
        std::string filename = argv[2];
        if (filename == "--help" || filename == "-h") {
            fmt::print("Usage: naab-lang check <file.naab>\n");
            fmt::print("  Type-check a .naab file without executing it.\n");
            return 0;
        }

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
            fmt::print("Usage: naab-lang fmt [--check] [--diff] [--config=path] <file.naab>\n");
            return 0;
        }

        // Parse flags
        bool check_only = false;
        bool show_diff = false;
        std::string config_file;
        std::string filename;

        for (int i = 2; i < argc; ++i) {
            std::string arg(argv[i]);
            if (arg == "--help" || arg == "-h") {
                fmt::print("Usage: naab-lang fmt [--check] [--diff] [--config=path] <file.naab>\n");
                fmt::print("  Format a .naab file in-place.\n");
                fmt::print("  --check   Check formatting without modifying\n");
                fmt::print("  --diff    Show diff without modifying\n");
                return 0;
            } else if (arg == "--check") {
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
                        // Generate unified diff between source and formatted
                        std::istringstream src_stream(source);
                        std::istringstream fmt_stream(formatted);
                        std::vector<std::string> src_lines, fmt_lines;
                        std::string line;
                        while (std::getline(src_stream, line)) src_lines.push_back(line);
                        while (std::getline(fmt_stream, line)) fmt_lines.push_back(line);
                        fmt::print("\n--- {}\n+++ {} (formatted)\n", filename, filename);
                        size_t si = 0, fi = 0;
                        while (si < src_lines.size() || fi < fmt_lines.size()) {
                            if (si < src_lines.size() && fi < fmt_lines.size() &&
                                src_lines[si] == fmt_lines[fi]) {
                                fmt::print(" {}\n", src_lines[si]);
                                si++; fi++;
                            } else {
                                // Find next matching line
                                size_t match_s = si, match_f = fi;
                                bool found = false;
                                for (size_t d = 1; d < 10 && !found; d++) {
                                    for (size_t ds = 0; ds <= d && !found; ds++) {
                                        size_t df = d - ds;
                                        if (si + ds < src_lines.size() && fi + df < fmt_lines.size() &&
                                            src_lines[si + ds] == fmt_lines[fi + df]) {
                                            match_s = si + ds; match_f = fi + df; found = true;
                                        }
                                    }
                                }
                                while (si < match_s) { fmt::print("-{}\n", src_lines[si++]); }
                                while (fi < match_f) { fmt::print("+{}\n", fmt_lines[fi++]); }
                                if (!found) {
                                    if (si < src_lines.size()) fmt::print("-{}\n", src_lines[si++]);
                                    if (fi < fmt_lines.size()) fmt::print("+{}\n", fmt_lines[fi++]);
                                }
                            }
                        }
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
        std::string api_key;
        size_t max_body = 1048576; // 1 MiB default
        unsigned int api_timeout = 10; // V-API-004 (R24): default 10s
        unsigned int api_rate_limit = 0; // V-DOS-005 (R25): 0 = disabled

        // Load govern.json API defaults from CWD
        std::vector<naab::governance::GovernanceRules::ApiKeyEntry> api_keys;
        {
            naab::governance::GovernanceEngine api_gov;
            if (api_gov.discoverAndLoad(std::filesystem::current_path().string())) {
                const auto& rules = api_gov.getRules();
                if (!rules.api.key.empty() && api_key.empty()) api_key = rules.api.key;
                if (rules.api.timeout != 10 && api_timeout == 10) api_timeout = static_cast<unsigned int>(rules.api.timeout);
                if (rules.api.rate_limit > 0 && api_rate_limit == 0) api_rate_limit = static_cast<unsigned int>(rules.api.rate_limit);
                if (rules.api.max_body != 1048576 && max_body == 1048576) max_body = rules.api.max_body;
                api_keys = rules.api.keys;
            }
        }

        // Scan all argv for api flags — CLI overrides govern.json
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--api-key" && i + 1 < argc) {
                api_key = argv[++i];
            } else if (arg == "--max-body" && i + 1 < argc) {
                try { max_body = static_cast<size_t>(std::stoull(argv[++i])); }
                catch (...) { fmt::print("Error: Invalid --max-body value\n"); return 1; }
            } else if (arg == "--api-timeout" && i + 1 < argc) {
                try { api_timeout = static_cast<unsigned int>(std::stoul(argv[++i])); }
                catch (...) { fmt::print("Error: Invalid --api-timeout value\n"); return 1; }
            } else if (arg == "--api-rate-limit" && i + 1 < argc) {
                try { api_rate_limit = static_cast<unsigned int>(std::stoul(argv[++i])); }
                catch (...) { fmt::print("Error: Invalid --api-rate-limit value\n"); return 1; }
            } else if (i == 2 && arg[0] != '-') {
                try { port = std::stoi(arg); }
                catch (...) { fmt::print("Error: Invalid port number\n"); return 1; }
            }
        }

        try {
            // Create REST API server
            naab::api::RestApiServer server(port, "0.0.0.0");

            // V-API-001: apply body size cap and optional API key auth
            server.setMaxBodySize(max_body);
            if (!api_key.empty()) server.setApiKey(api_key);
            // Multi-key with scoped permissions (from govern.json api.keys[])
            if (!api_keys.empty()) server.setApiKeys(api_keys);
            // V-API-004 (R24): per-request execution timeout
            server.setApiTimeout(api_timeout);
            // V-DOS-005 (R25): per-client rate limiting
            if (api_rate_limit > 0) server.setApiRateLimit(api_rate_limit);

            // Set up block loader for API endpoints
            std::string db_path = NAAB_DATABASE_PATH;
            auto loader = std::make_shared<naab::runtime::BlockLoader>(db_path);
            server.setBlockLoader(loader);

            // Create and set up interpreter for code execution
            auto interpreter = std::make_shared<naab::interpreter::Interpreter>();
            server.setInterpreter(interpreter);

            // V-SC-005: apply lockfile verification before starting the server.
            // The run command has this gate; the api command was missing it.
            if (global_lock_check) {
                std::string lf_path_api = global_lock_path;
                if (lf_path_api.empty()) {
                    auto cwd_api = std::filesystem::current_path().string();
                    lf_path_api = naab::discoverLockfilePath(cwd_api);
                    if (lf_path_api.empty()) {
                        lf_path_api = cwd_api + "/.naab/naab.lock";
                    }
                }
                if (!naab::Lockfile::verifySignature(lf_path_api)) {
                    fprintf(stderr, "[lock] TAMPER DETECTED: lockfile signature mismatch.\n"
                                    "  The lockfile may have been modified by an attacker.\n"
                                    "  Re-run with --lock to regenerate.\n");
                    fflush(stderr);
                    _exit(1);
                }
                auto& lang_registry_api = naab::runtime::LanguageRegistry::instance();
                std::unordered_map<std::string, std::string> observed_api;
                for (const auto& lang : lang_registry_api.supportedLanguages()) {
                    auto* exec = lang_registry_api.getExecutor(lang);
                    if (exec) {
                        std::string ver = exec->getRuntimeVersion();
                        if (!ver.empty()) observed_api[lang] = ver;
                    }
                }
                naab::Lockfile loaded_lf_api = naab::Lockfile::load(lf_path_api);
                auto drift_api = loaded_lf_api.checkDrift(observed_api);
                if (!drift_api.empty()) {
                    fprintf(stderr, "[lock] RUNTIME DRIFT DETECTED:\n");
                    for (const auto& d : drift_api) fprintf(stderr, "  %s\n", d.c_str());
                    fprintf(stderr, "  Re-run with --lock to update: naab-lang --lock api\n");
                    fflush(stderr);
                    _exit(1);
                }
            }

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
        // Parse init subflags
        bool no_governance = false;
        bool governance_only = false;
        bool force = false;
        bool taint_flag = false;
        bool package_mode = false;
        std::string preset;
        std::string languages_override;

        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--package") package_mode = true;
            else if (arg == "--no-governance") no_governance = true;
            else if (arg == "--governance") governance_only = true;
            else if (arg == "--force") force = true;
            else if (arg == "--taint") taint_flag = true;
            else if (arg == "--governance-preset" && i + 1 < argc) preset = argv[++i];
            else if (arg == "--languages" && i + 1 < argc) languages_override = argv[++i];
        }

        fmt::print("\n");
        fmt::print("───────────────────────────────────────\n");
        fmt::print("  NAAb {} Initializer\n", package_mode ? "Package" : "Project");
        fmt::print("───────────────────────────────────────\n\n");

        // Package mode: create a full package project structure
        if (package_mode) {
            namespace fs = std::filesystem;
            std::string pkg_name = fs::current_path().filename().string();

            // naab.toml with package metadata
            if (!fs::exists("naab.toml") || force) {
                std::ofstream f("naab.toml");
                f << "[package]\n"
                  << "name = \"" << pkg_name << "\"\n"
                  << "version = \"0.1.0\"\n"
                  << "description = \"A NAAb package\"\n"
                  << "license = \"MIT\"\n"
                  << "keywords = []\n\n"
                  << "[exports]\n"
                  << "main = \"src/lib.naab\"\n\n"
                  << "[package.governance]\n"
                  << "plugin_file = \"governance/checks.naab\"\n"
                  << "rules_file = \"governance/rules.json\"\n\n"
                  << "[dependencies]\n";
                f.close();
                fmt::print("✓ Created naab.toml (package manifest)\n");
            } else {
                fmt::print("  naab.toml already exists (skipped)\n");
            }

            // Source directory
            fs::create_directories("src");
            if (!fs::exists("src/lib.naab") || force) {
                std::ofstream f("src/lib.naab");
                f << "// " << pkg_name << " - Main entry point\n"
                  << "// Exported functions are available to importers\n\n"
                  << "function hello(name) {\n"
                  << "    return \"Hello from " << pkg_name << ", \" + name + \"!\"\n"
                  << "}\n";
                f.close();
                fmt::print("✓ Created src/lib.naab (entry point)\n");
            }

            // Governance directory
            fs::create_directories("governance");
            if (!fs::exists("governance/checks.naab") || force) {
                std::ofstream f("governance/checks.naab");
                f << "// Governance plugin checks for " << pkg_name << "\n"
                  << "// These run automatically when your package is installed\n\n"
                  << "function check_example(context) {\n"
                  << "    // context is a dict with: code, language, function_name, file\n"
                  << "    // Return: { \"passed\": true/false, \"message\": \"...\" }\n"
                  << "    return { \"passed\": true, \"message\": \"Check passed\" }\n"
                  << "}\n";
                f.close();
                fmt::print("✓ Created governance/checks.naab (governance plugin)\n");
            }
            if (!fs::exists("governance/rules.json") || force) {
                std::ofstream f("governance/rules.json");
                f << "[\n"
                  << "  {\n"
                  << "    \"id\": \"" << pkg_name << ".EXAMPLE-001\",\n"
                  << "    \"function\": \"check_example\",\n"
                  << "    \"description\": \"Example governance check\",\n"
                  << "    \"level\": \"advisory\",\n"
                  << "    \"trigger\": \"polyglot_block\"\n"
                  << "  }\n"
                  << "]\n";
                f.close();
                fmt::print("✓ Created governance/rules.json (rule definitions)\n");
            }

            // Tests directory
            fs::create_directories("tests");
            if (!fs::exists("tests/test_lib.naab") || force) {
                std::ofstream f("tests/test_lib.naab");
                f << "// Tests for " << pkg_name << "\n"
                  << "import \"../src/lib.naab\" as lib\n\n"
                  << "function test_hello() {\n"
                  << "    let result = lib.hello(\"Test\")\n"
                  << "    let expected = \"Hello from " << pkg_name << ", Test!\"\n"
                  << "    if result == expected {\n"
                  << "        return [1, 1]\n"
                  << "    } else {\n"
                  << "        io.write(\"FAIL: Expected '\" + expected + \"' got '\" + result + \"'\")\n"
                  << "        return [0, 1]\n"
                  << "    }\n"
                  << "}\n\n"
                  << "main {\n"
                  << "    let r = test_hello()\n"
                  << "    io.write(r[0] + \"/\" + r[1] + \" tests passed\")\n"
                  << "}\n";
                f.close();
                fmt::print("✓ Created tests/test_lib.naab\n");
            }

            // Examples directory
            fs::create_directories("examples");
            if (!fs::exists("examples/basic.naab") || force) {
                std::ofstream f("examples/basic.naab");
                f << "// Example: Using " << pkg_name << "\n"
                  << "import \"../src/lib.naab\" as lib\n\n"
                  << "main {\n"
                  << "    let msg = lib.hello(\"World\")\n"
                  << "    io.write(msg)\n"
                  << "}\n";
                f.close();
                fmt::print("✓ Created examples/basic.naab\n");
            }

            // CLAUDE.md
            if (!fs::exists("CLAUDE.md") || force) {
                std::ofstream f("CLAUDE.md");
                f << "# " << pkg_name << "\n\n"
                  << "## What this package does\n\n"
                  << "[Describe what this package does in one paragraph]\n\n"
                  << "## Key functions\n\n"
                  << "- `hello(name)` - Returns a greeting string\n\n"
                  << "## Usage\n\n"
                  << "```naab\n"
                  << "import \"" << pkg_name << "\" as pkg\n\n"
                  << "main {\n"
                  << "    let result = pkg.hello(\"World\")\n"
                  << "    io.write(result)\n"
                  << "}\n"
                  << "```\n\n"
                  << "## Governance rules\n\n"
                  << "This package ships governance rules that auto-apply on install.\n"
                  << "See `governance/rules.json` for details.\n";
                f.close();
                fmt::print("✓ Created CLAUDE.md (AI instructions)\n");
            }

            fmt::print("\n  Package '{}' initialized!\n\n", pkg_name);
            fmt::print("  Next steps:\n");
            fmt::print("    1. Edit src/lib.naab with your package code\n");
            fmt::print("    2. Add governance checks in governance/checks.naab\n");
            fmt::print("    3. Update CLAUDE.md for AI users\n");
            fmt::print("    4. Push to GitHub and create a release\n");
            fmt::print("    5. Run: naab-lang publish\n\n");
            return 0;
        }

        // Step 1: Create naab.toml (unless --governance only)
        if (!governance_only) {
            if (naab::manifest::createDefaultManifest("naab.toml")) {
                fmt::print("✓ Created naab.toml\n");
            } else {
                fmt::print("  naab.toml already exists (skipped)\n");
            }
        }

        // Step 2: Governance setup
        if (!no_governance) {
            if (!preset.empty()) {
                // Non-interactive preset mode
                auto config = naab::cli::presetToConfig(preset, languages_override, taint_flag);
                fmt::print("\n");
                if (!naab::cli::writeGovernJson("govern.json", config, force)) {
                    return 1;
                }
            } else {
                // Interactive mode
                bool is_tty = isatty(fileno(stdin));
                if (is_tty && !governance_only) {
                    fmt::print("\nSet up governance? (govern.json catches AI code mistakes at runtime)\n");
                    fmt::print("[Y/n]: ");
                    std::string line;
                    std::getline(std::cin, line);
                    if (!line.empty() && (line[0] == 'n' || line[0] == 'N')) {
                        fmt::print("\n  Skipping governance setup.\n");
                        fmt::print("  Run 'naab-lang init --governance' later to add it.\n\n");
                        return 0;
                    }
                }
                auto config = naab::cli::runInteractiveSetup(is_tty);
                if (!naab::cli::writeGovernJson("govern.json", config, force)) {
                    return 1;
                }
            }

            fmt::print("\n  Next steps:\n");
            fmt::print("    naab-lang your_file.naab          Run with governance\n");
            fmt::print("    naab-lang --governance-verbose    See all check results\n");
            fmt::print("    Edit govern.json to customize     Full reference: govern-template.json\n\n");
        }

        return 0;

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

    } else if (command == "calibrate") {
        // ================================================================
        // naab-lang calibrate — Benchmark installed languages
        // ================================================================
        namespace fs = std::filesystem;

        // Parse options
        std::vector<std::string> filter_languages;
        std::vector<std::string> filter_tasks;
        int iterations = 3;
        std::string output_path;

        for (int i = command_arg_index + 1; i < argc; ++i) {
            std::string arg(argv[i]);
            if (arg == "--languages" && i + 1 < argc) {
                std::string langs = argv[++i];
                std::istringstream iss(langs);
                std::string lang;
                while (std::getline(iss, lang, ',')) filter_languages.push_back(lang);
            } else if (arg == "--tasks" && i + 1 < argc) {
                std::string tasks = argv[++i];
                std::istringstream iss(tasks);
                std::string task;
                while (std::getline(iss, task, ',')) filter_tasks.push_back(task);
            } else if (arg == "--iterations" && i + 1 < argc) {
                try { iterations = std::stoi(argv[++i]); } catch (const std::exception&) {
                    fprintf(stderr, "Error: --iterations requires a numeric value\n"); return 1;
                }
            } else if (arg == "--output" && i + 1 < argc) {
                output_path = argv[++i];
            }
        }

        // Find benchmark directory
        std::string exe_dir;
        {
            std::error_code ec;
            auto exe = fs::canonical(argv[0], ec);
            exe_dir = exe.parent_path().parent_path().string(); // up from build/
        }
        std::string bench_dir = exe_dir + "/benchmarks";
        if (!fs::exists(bench_dir)) {
            fmt::print(stderr, "Error: Benchmark directory not found: {}\n", bench_dir);
            fmt::print(stderr, "  Expected at: {}\n", bench_dir);
            _exit(1);
        }

        // Detect installed languages
        struct LangInfo {
            std::string name;
            std::string ext;
            std::string runner;  // empty = compiled
        };
        std::vector<LangInfo> languages = {
            {"python", ".py", "python3"},
            {"javascript", ".js", "node"},
            {"ruby", ".rb", "ruby"},
            {"go", ".go", ""},
            {"rust", ".rs", ""},
            {"nim", ".nim", ""},
            {"shell", ".sh", "bash"},
        };

        // Check which languages are available
        std::vector<LangInfo> available;
        for (auto& lang : languages) {
            if (!filter_languages.empty()) {
                bool found = false;
                for (auto& fl : filter_languages)
                    if (fl == lang.name) found = true;
                if (!found) continue;
            }
            // Check if runner/compiler exists
            std::string check_cmd;
            if (!lang.runner.empty())
                check_cmd = "which " + lang.runner + " >/dev/null 2>&1";
            else if (lang.name == "go")
                check_cmd = "which go >/dev/null 2>&1";
            else if (lang.name == "rust")
                check_cmd = "which rustc >/dev/null 2>&1";
            else if (lang.name == "nim")
                check_cmd = "which nim >/dev/null 2>&1";
            else
                continue;
            if (system(check_cmd.c_str()) == 0) {
                available.push_back(lang);
            }
        }

        if (available.empty()) {
            fmt::print("No supported languages found. Install python3, node, go, rustc, nim, or ruby.\n");
            _exit(1);
        }

        fmt::print("NAAb Calibration\n");
        fmt::print("================\n");
        fmt::print("Languages: ");
        for (size_t i = 0; i < available.size(); ++i) {
            if (i > 0) fmt::print(", ");
            fmt::print("{}", available[i].name);
        }
        fmt::print("\nIterations: {}\n\n", iterations);

        // Task categories = subdirectories in benchmarks/
        std::vector<std::string> task_categories;
        for (auto& entry : fs::directory_iterator(bench_dir)) {
            if (entry.is_directory()) {
                std::string name = entry.path().filename().string();
                if (!filter_tasks.empty()) {
                    bool found = false;
                    for (auto& ft : filter_tasks)
                        if (ft == name) found = true;
                    if (!found) continue;
                }
                task_categories.push_back(name);
            }
        }
        std::sort(task_categories.begin(), task_categories.end());

        // Results structure: results[task/bench][lang] = median_us
        struct BenchResult { int64_t us; };
        std::map<std::string, std::map<std::string, BenchResult>> results;

        for (auto& task : task_categories) {
            fmt::print("Task: {}\n", task);
            std::string task_dir = bench_dir + "/" + task;

            for (auto& lang : available) {
                for (auto& bench_entry : fs::directory_iterator(task_dir)) {
                    std::string bench_file = bench_entry.path().string();
                    if (bench_file.size() < lang.ext.size()) continue;
                    if (bench_file.substr(bench_file.size() - lang.ext.size()) != lang.ext) continue;

                    std::string bench_name = bench_entry.path().stem().string();

                    // Reject filenames with non-safe characters (whitelist approach)
                    bool unsafe = false;
                    for (char c : bench_file) {
                        if (!std::isalnum(static_cast<unsigned char>(c)) &&
                            c != '.' && c != '-' && c != '_' && c != '/') {
                            unsafe = true;
                            break;
                        }
                    }
                    if (unsafe) {
                        fmt::print(stderr, "  Skipping unsafe filename: {}\n", bench_file);
                        continue;
                    }

                    // Shell-quote the filename for use in popen() commands
                    std::string quoted = "'" + bench_file + "'";

                    std::vector<int64_t> times;
                    for (int iter = 0; iter < iterations; ++iter) {
                        std::string cmd;
                        std::string tmp_bin;

                        if (!lang.runner.empty()) {
                            cmd = lang.runner + " " + quoted + " 2>/dev/null";
                        } else if (lang.name == "go") {
                            cmd = "go run " + quoted + " 2>/dev/null";
                        } else if (lang.name == "rust") {
                            tmp_bin = "/data/data/com.termux/files/usr/tmp/naab_bench_" +
                                      std::to_string(getpid());
                            cmd = "rustc -o " + tmp_bin + " " + quoted +
                                  " 2>/dev/null && " + tmp_bin;
                        } else if (lang.name == "nim") {
                            tmp_bin = "/data/data/com.termux/files/usr/tmp/naab_bench_" +
                                      std::to_string(getpid());
                            cmd = "nim c --hints:off -o:" + tmp_bin + " " + quoted +
                                  " 2>/dev/null && " + tmp_bin;
                        }

                        std::unique_ptr<FILE, decltype(&pclose)> pipe(
                            popen(cmd.c_str(), "r"), pclose);
                        if (!pipe) continue;
                        char buf[256];
                        std::string output;
                        while (fgets(buf, sizeof(buf), pipe.get()))
                            output += buf;
                        int status = pclose(pipe.release());

                        if (!tmp_bin.empty()) std::remove(tmp_bin.c_str());

                        if (status == 0 && !output.empty()) {
                            try {
                                output.erase(output.find_last_not_of(" \t\n\r") + 1);
                                int64_t us = std::stoll(output);
                                if (us > 0) times.push_back(us);
                            } catch (...) {}
                        }
                    }

                    if (times.empty()) {
                        fmt::print("  {} / {} — FAILED\n", lang.name, bench_name);
                        continue;
                    }

                    std::sort(times.begin(), times.end());
                    int64_t median = times[times.size() / 2];
                    fmt::print("  {} / {} — {} us\n", lang.name, bench_name, median);

                    std::string key = task + "/" + bench_name;
                    results[key][lang.name] = {median};
                }
            }
            fmt::print("\n");
        }

        // Aggregate results by task category
        struct CatScore { int64_t us; int score; };
        std::map<std::string, std::map<std::string, CatScore>> cat_results;

        for (auto& task : task_categories) {
            std::map<std::string, std::vector<int64_t>> lang_times;
            for (auto& [key, lang_map] : results) {
                if (key.substr(0, task.size() + 1) == task + "/") {
                    for (auto& [lang, br] : lang_map) {
                        lang_times[lang].push_back(br.us);
                    }
                }
            }

            std::map<std::string, int64_t> avg_times;
            int64_t fastest = INT64_MAX;
            for (auto& [lang, tvec] : lang_times) {
                int64_t sum = 0;
                for (auto t : tvec) sum += t;
                avg_times[lang] = sum / (int64_t)tvec.size();
                if (avg_times[lang] < fastest) fastest = avg_times[lang];
            }

            for (auto& [lang, avg] : avg_times) {
                int score = (fastest > 0) ? (int)(100.0 * (double)fastest / (double)avg) : 0;
                cat_results[task][lang] = {avg, score};
            }
        }

        // Build machine string
        std::string machine_str =
#ifdef __aarch64__
            "aarch64"
#elif defined(__x86_64__)
            "x86_64"
#else
            "unknown"
#endif
#ifdef __ANDROID__
            "-android"
#elif defined(__linux__)
            "-linux"
#elif defined(__APPLE__)
            "-macos"
#endif
        ;

        auto now = std::chrono::system_clock::now();
        auto ts = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();

        // Write calibration file as JSON manually
        if (output_path.empty()) {
            const char* home = std::getenv("HOME");
            if (home) {
                output_path = std::string(home) + "/.naab/calibration.json";
            } else {
                output_path = "calibration.json";
            }
        }

        {
            auto parent = fs::path(output_path).parent_path();
            if (!parent.empty()) fs::create_directories(parent);
        }

        std::ofstream out(output_path);
        if (!out.is_open()) {
            fmt::print(stderr, "Error: Could not write to {}\n", output_path);
            _exit(1);
        }

        out << "{\n";
        out << "  \"machine\": \"" << machine_str << "\",\n";
        out << "  \"timestamp\": " << ts << ",\n";
        out << "  \"languages_tested\": [";
        for (size_t i = 0; i < available.size(); ++i) {
            if (i > 0) out << ", ";
            out << "\"" << available[i].name << "\"";
        }
        out << "],\n";
        out << "  \"results\": {\n";
        bool first_task = true;
        for (auto& [task, lang_map] : cat_results) {
            if (!first_task) out << ",\n";
            first_task = false;
            out << "    \"" << task << "\": {\n";
            bool first_lang = true;
            for (auto& [lang, cs] : lang_map) {
                if (!first_lang) out << ",\n";
                first_lang = false;
                out << "      \"" << lang << "\": {\"us\": " << cs.us
                    << ", \"score\": " << cs.score << "}";
            }
            out << "\n    }";
        }
        out << "\n  }\n}\n";
        out.close();

        fmt::print("Calibration saved to: {}\n", output_path);

        // Print summary
        fmt::print("\nCalibration Summary\n");
        fmt::print("===================\n");
        for (auto& [task, lang_map] : cat_results) {
            fmt::print("\n{}:\n", task);
            std::vector<std::pair<std::string, int>> scored;
            for (auto& [lang, cs] : lang_map) {
                scored.push_back({lang, cs.score});
            }
            std::sort(scored.begin(), scored.end(),
                [](auto& a, auto& b) { return a.second > b.second; });

            int rank = 1;
            for (auto& [lang, score] : scored) {
                int64_t us = cat_results[task][lang].us;
                size_t bar_len = (size_t)(score / 5);
                std::string bar(bar_len, '#');
                std::string pad(20 - bar_len, ' ');
                fmt::print("  #{} {:>12} {:>8} us  {}{}  {}\n",
                    rank++, lang, us, bar, pad, score);
            }
        }

    } else if (command == "race") {
        // ================================================================
        // naab-lang race — Race languages against each other
        // ================================================================

        // Parse options
        std::vector<std::string> race_languages;
        int block_index = -1;
        std::string race_file;

        for (int i = command_arg_index + 1; i < argc; ++i) {
            std::string arg(argv[i]);
            if (arg == "--languages" && i + 1 < argc) {
                std::string langs = argv[++i];
                std::istringstream iss(langs);
                std::string lang;
                while (std::getline(iss, lang, ',')) race_languages.push_back(lang);
            } else if (arg == "--block" && i + 1 < argc) {
                try { block_index = std::stoi(argv[++i]); } catch (const std::exception&) {
                    fprintf(stderr, "Error: --block requires a numeric value\n"); return 1;
                }
            } else if (arg.size() > 5 && arg.substr(arg.size()-5) == ".naab") {
                race_file = arg;
            }
        }

        if (race_file.empty()) {
            fmt::print("Usage: naab-lang race <file.naab> --block N [--languages python,go,rust]\n\n");
            fmt::print("  Races a specific polyglot block from your script against alternative languages.\n");
            fmt::print("  Uses calibration data from ~/.naab/calibration.json if available.\n\n");
            fmt::print("  Options:\n");
            fmt::print("    --block N        Block index (0-based) to race\n");
            fmt::print("    --languages L    Comma-separated list of languages to race\n\n");
            fmt::print("  Example:\n");
            fmt::print("    naab-lang race my_script.naab --block 0 --languages python,go,nim\n");
            _exit(0);
        }

        // Load calibration data using GovernanceEngine's loader
        naab::governance::GovernanceEngine cal_engine;
        // Temporarily set calibration config to load from default path
        cal_engine.getMutableRules().polyglot_optimization.calibration.enabled = true;
        bool has_cal = cal_engine.loadCalibration();

        const char* home = std::getenv("HOME");
        std::string cal_path = home ? std::string(home) + "/.naab/calibration.json" : "calibration.json";

        if (!has_cal) {
            fmt::print("No calibration data found at {}\n", cal_path);
            fmt::print("Run 'naab-lang calibrate' first to benchmark your machine.\n\n");
            fmt::print("This will measure actual performance of installed languages\n");
            fmt::print("so race results are based on your hardware, not estimates.\n");
            _exit(1);
        }

        // Parse the .naab file to find polyglot blocks
        try {
            std::string source = read_file(race_file);
            naab::lexer::Lexer lexer(source);
            auto tokens = lexer.tokenize();

            struct PolyglotBlock {
                std::string language;
                std::string code;
                int line;
            };
            std::vector<PolyglotBlock> blocks;
            for (size_t i = 0; i < tokens.size(); ++i) {
                if (tokens[i].type == naab::lexer::TokenType::INLINE_CODE) {
                    std::string val = tokens[i].value;
                    // Token format is "language[vars]->TYPE:code" or "language:code"
                    // Find the first ':' that separates language header from code
                    auto colon = val.find(':');
                    std::string lang, code;
                    if (colon != std::string::npos) {
                        std::string header = val.substr(0, colon);
                        code = val.substr(colon + 1);
                        // Extract just the language name (before [ or ->)
                        auto bracket = header.find('[');
                        auto arrow = header.find("->");
                        size_t end = header.size();
                        if (bracket != std::string::npos && bracket < end) end = bracket;
                        if (arrow != std::string::npos && arrow < end) end = arrow;
                        lang = header.substr(0, end);
                    } else {
                        lang = val;
                        code = "";
                    }
                    // Strip leading/trailing whitespace from code
                    while (!code.empty() && (code[0] == ' ' || code[0] == '\n' || code[0] == '\r'))
                        code.erase(code.begin());
                    while (!code.empty() && (code.back() == ' ' || code.back() == '\n' || code.back() == '\r'))
                        code.pop_back();
                    blocks.push_back({lang, code, tokens[i].line});
                }
            }

            if (blocks.empty()) {
                fmt::print("No polyglot blocks found in {}\n", race_file);
                _exit(1);
            }

            if (block_index < 0) {
                fmt::print("Polyglot blocks in {}:\n\n", race_file);
                for (size_t i = 0; i < blocks.size(); ++i) {
                    std::string preview = blocks[i].code.substr(0, 60);
                    if (preview.find('\n') != std::string::npos)
                        preview = preview.substr(0, preview.find('\n'));
                    fmt::print("  [{}] <<{}>> line {} — {}\n",
                        i, blocks[i].language, blocks[i].line, preview);
                }
                fmt::print("\nUse --block N to race a specific block.\n");
                _exit(0);
            }

            if (block_index >= (int)blocks.size()) {
                fmt::print("Block index {} out of range (0-{})\n",
                    block_index, blocks.size() - 1);
                _exit(1);
            }

            auto& block = blocks[(size_t)block_index];
            fmt::print("Racing block [{}]: <<{}>> at line {}\n", block_index,
                block.language, block.line);
            std::string code_preview = block.code.substr(0, 80);
            // Show first line only for preview
            if (code_preview.find('\n') != std::string::npos)
                code_preview = code_preview.substr(0, code_preview.find('\n')) + "...";
            fmt::print("Code: {}\n\n", code_preview);

            // Detect task category from code content
            std::string code_lower = block.code;
            for (auto& c : code_lower) c = (char)tolower(c);
            std::string detected_task;
            // Numerical detection
            if (code_lower.find("sort") != std::string::npos ||
                code_lower.find("matrix") != std::string::npos ||
                code_lower.find("sum(") != std::string::npos ||
                code_lower.find("range(") != std::string::npos ||
                code_lower.find("math.") != std::string::npos ||
                code_lower.find("sqrt") != std::string::npos ||
                code_lower.find("float") != std::string::npos)
                detected_task = "numerical";
            else if (code_lower.find("regex") != std::string::npos ||
                code_lower.find("split") != std::string::npos ||
                code_lower.find("concat") != std::string::npos ||
                code_lower.find("replace") != std::string::npos ||
                code_lower.find("strip") != std::string::npos ||
                code_lower.find("upper") != std::string::npos ||
                code_lower.find("lower") != std::string::npos ||
                code_lower.find("string") != std::string::npos)
                detected_task = "string";
            else if (code_lower.find("open(") != std::string::npos ||
                code_lower.find("readfile") != std::string::npos ||
                code_lower.find("writefile") != std::string::npos ||
                code_lower.find("fs.") != std::string::npos ||
                code_lower.find("os.walk") != std::string::npos ||
                code_lower.find("readdir") != std::string::npos)
                detected_task = "file_io";
            else if (code_lower.find("json") != std::string::npos ||
                code_lower.find("parse") != std::string::npos ||
                code_lower.find("stringify") != std::string::npos)
                detected_task = "json";
            else if (code_lower.find("thread") != std::string::npos ||
                code_lower.find("worker") != std::string::npos ||
                code_lower.find("async") != std::string::npos ||
                code_lower.find("goroutine") != std::string::npos ||
                code_lower.find("channel") != std::string::npos)
                detected_task = "concurrency";
            else if (code_lower.find("http") != std::string::npos ||
                code_lower.find("fetch") != std::string::npos ||
                code_lower.find("socket") != std::string::npos ||
                code_lower.find("request") != std::string::npos)
                detected_task = "web_apis";
            else if (code_lower.find("echo") != std::string::npos ||
                code_lower.find("pipe") != std::string::npos ||
                code_lower.find("stdin") != std::string::npos ||
                code_lower.find("argv") != std::string::npos ||
                code_lower.find("csv") != std::string::npos)
                detected_task = "cli";
            else if (code_lower.find("alloc") != std::string::npos ||
                code_lower.find("malloc") != std::string::npos ||
                code_lower.find("subprocess") != std::string::npos ||
                code_lower.find("spawn") != std::string::npos)
                detected_task = "systems";

            auto& cal_data = cal_engine.getCalibrationData();

            // Helper to print a task category
            auto print_task = [&](const std::string& task, bool is_primary) {
                auto it = cal_data.find(task);
                if (it == cal_data.end()) return;
                auto& lang_entries = it->second;

                std::vector<std::pair<std::string, int>> scored;
                for (auto& [lang, entry] : lang_entries) {
                    if (!race_languages.empty()) {
                        bool found = (lang == block.language);
                        for (auto& rl : race_languages) if (rl == lang) found = true;
                        if (!found) continue;
                    }
                    scored.push_back({lang, entry.score});
                }
                std::sort(scored.begin(), scored.end(),
                    [](auto& a, auto& b) { return a.second > b.second; });

                if (is_primary) {
                    fmt::print("  RACE RESULTS: {} (calibrated on this machine)\n", task);
                    fmt::print("  {}\n\n", std::string(50, '='));
                } else {
                    fmt::print("  {}:\n", task);
                }

                int rank = 1;
                int current_rank = 0;
                int current_score = 0;
                std::string best_lang;
                int64_t best_us = 0;
                int64_t current_us = 0;

                for (auto& [lang, score] : scored) {
                    int64_t us = lang_entries.at(lang).us;
                    size_t bar_len = (size_t)(score / 5);
                    std::string bar(bar_len, '#');
                    std::string pad(20 - bar_len, ' ');
                    bool is_current = (lang == block.language);
                    std::string marker = is_current ? "  <-- YOU" : "";
                    if (is_current) {
                        current_rank = rank;
                        current_score = score;
                        current_us = us;
                    }
                    if (rank == 1) { best_lang = lang; best_us = us; }
                    fmt::print("  #{} {:>12} {:>8} us  {}{}  {}{}\n",
                        rank++, lang, us, bar, pad, score, marker);
                }

                if (is_primary && current_rank > 0) {
                    fmt::print("\n");
                    if (current_rank == 1) {
                        fmt::print("  You're using the fastest language for this task!\n");
                    } else {
                        double speedup = (current_us > 0 && best_us > 0) ?
                            (double)current_us / (double)best_us : 0;
                        fmt::print("  Your block uses: {} (score: {}/100, rank: #{})\n",
                            block.language, current_score, current_rank);
                        fmt::print("  Fastest option:  {} ({:.1f}x faster)\n",
                            best_lang, speedup);
                    }
                }
                fmt::print("\n");
            };

            // Show primary detected task first
            if (!detected_task.empty()) {
                fmt::print("Detected task type: {}\n\n", detected_task);
                print_task(detected_task, true);

                // Show other categories briefly
                fmt::print("Other categories (for reference):\n\n");
                for (auto& [task, _] : cal_data) {
                    if (task != detected_task) print_task(task, false);
                }
            } else {
                fmt::print("Could not auto-detect task type. Showing all categories:\n\n");
                for (auto& [task, _] : cal_data) {
                    print_task(task, false);
                }
            }

        } catch (const std::exception& e) {
            fmt::print("Error: {}\n", e.what());
            _exit(1);
        }

    } else if (command == "install") {
        // ================================================================
        // naab-lang install [spec] — Install packages
        // ================================================================
        naab::packages::PackageManager pm(".");

        // Collect positional args (package specs), skip flags
        std::vector<std::string> specs;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a.rfind("--", 0) == 0 || a.rfind("-", 0) == 0) {
                // Skip known flags silently; warn on unknown
                if (a != "--verbose" && a != "-v" && a != "--governance-verbose") {
                    fmt::print(stderr, "Warning: Unknown flag '{}' ignored\n", a);
                }
                continue;
            }
            specs.push_back(a);
        }

        if (specs.empty()) {
            // No positional args → install all from naab.toml
            if (pm.installAll()) {
                // installAll prints its own output
            } else {
                fmt::print(stderr, "Error: {}\n", pm.getLastError());
                fflush(stderr);
                _exit(1);
            }
        } else {
            for (const auto& spec : specs) {
                if (pm.install(spec)) {
                    // install prints its own output
                } else {
                    fmt::print(stderr, "Error: {}\n", pm.getLastError());
                    fflush(stderr);
                    _exit(1);
                }
            }
        }

    } else if (command == "remove") {
        // ================================================================
        // naab-lang remove <package> — Remove installed package
        // ================================================================
        if (argc < 3) {
            fmt::print(stderr, "Error: Missing package name\n");
            fmt::print(stderr, "Usage: naab-lang remove <package-name>\n");
            fflush(stderr);
            _exit(1);
        }
        naab::packages::PackageManager pm(".");
        std::string name = argv[2];
        if (pm.remove(name)) {
            // remove prints its own output
        } else {
            fmt::print(stderr, "Error: {}\n", pm.getLastError());
            fflush(stderr);
            _exit(1);
        }

    } else if (command == "update") {
        // ================================================================
        // naab-lang update [package] — Update packages
        // ================================================================
        naab::packages::PackageManager pm(".");
        std::string name = (argc >= 3) ? argv[2] : "";
        if (pm.update(name)) {
            // update prints its own output
        } else {
            fmt::print(stderr, "Error: {}\n", pm.getLastError());
            fflush(stderr);
            _exit(1);
        }

    } else if (command == "list") {
        // ================================================================
        // naab-lang list — List installed packages
        // ================================================================
        naab::packages::PackageManager pm(".");
        auto packages = pm.list();
        if (packages.empty()) {
            fmt::print("No packages installed.\n");
            fmt::print("  Install one with: naab-lang install user/repo\n");
        } else {
            fmt::print("Installed packages:\n\n");
            for (const auto& pkg : packages) {
                fmt::print("  {} {}", pkg.name, pkg.version);
                if (pkg.has_governance) fmt::print(" [governance]");
                if (pkg.has_claude_md) fmt::print(" [CLAUDE.md]");
                fmt::print("\n");
                if (!pkg.description.empty()) {
                    fmt::print("    {}\n", pkg.description);
                }
                fmt::print("    → naab_modules/{}/\n\n", pkg.name);
            }
            fmt::print("{} package(s) installed.\n", packages.size());
        }

    } else if (command == "info") {
        // ================================================================
        // naab-lang info <user/repo> — Show package info
        // ================================================================
        if (argc < 3) {
            fmt::print(stderr, "Error: Missing package spec\n");
            fmt::print(stderr, "Usage: naab-lang info <user/repo>\n");
            fflush(stderr);
            _exit(1);
        }
        naab::packages::PackageManager pm(".");
        std::string spec = argv[2];
        auto pkg = pm.info(spec);
        if (pkg.name.empty()) {
            fmt::print(stderr, "Error: {}\n", pm.getLastError());
            fflush(stderr);
            _exit(1);
        }
        fmt::print("{} v{}\n", pkg.name, pkg.version);
        if (!pkg.description.empty()) fmt::print("  {}\n", pkg.description);
        if (!pkg.github.empty()) fmt::print("  GitHub: {}\n", pkg.github);
        if (!pkg.license.empty()) fmt::print("  License: {}\n", pkg.license);
        if (!pkg.keywords.empty()) {
            fmt::print("  Keywords: ");
            for (size_t i = 0; i < pkg.keywords.size(); i++) {
                if (i > 0) fmt::print(", ");
                fmt::print("{}", pkg.keywords[i]);
            }
            fmt::print("\n");
        }
        if (pkg.has_governance) fmt::print("  Governance: ships governance rules\n");
        if (pkg.has_claude_md) fmt::print("  AI-ready: includes CLAUDE.md\n");
        if (!pkg.main_file.empty()) fmt::print("  Entry point: {}\n", pkg.main_file);
        if (!pkg.dependencies.empty()) {
            fmt::print("  Dependencies:\n");
            for (const auto& dep : pkg.dependencies) {
                fmt::print("    - {} ({})\n", dep.name,
                           dep.is_path_dep ? dep.path : dep.github + "@" + dep.version_spec);
            }
        }

    } else if (command == "search") {
        // ================================================================
        // naab-lang search <query> — Search package registry
        // ================================================================
        if (argc < 3) {
            fmt::print(stderr, "Error: Missing search query\n");
            fmt::print(stderr, "Usage: naab-lang search <query>\n");
            fflush(stderr);
            _exit(1);
        }
        naab::packages::PackageManager pm(".");
        // Join all remaining args as query
        std::string query;
        for (int i = 2; i < argc; i++) {
            if (i > 2) query += " ";
            query += argv[i];
        }
        auto results = pm.search(query);
        if (results.empty()) {
            fmt::print("No packages found matching '{}'.\n", query);
        } else {
            fmt::print("Search results for '{}':\n\n", query);
            for (const auto& pkg : results) {
                fmt::print("  {} v{}\n", pkg.github, pkg.version);
                if (!pkg.description.empty()) {
                    fmt::print("    {}\n", pkg.description);
                }
                if (pkg.has_governance) fmt::print("    [ships governance rules]\n");
                fmt::print("\n");
            }
            fmt::print("{} package(s) found. Install with: naab-lang install <user/repo>\n", results.size());
        }

    } else if (command == "publish") {
        // ================================================================
        // naab-lang publish — Validate package for publishing
        // ================================================================
        naab::packages::PackageManager pm(".");
        if (!pm.publish()) {
            // publish() prints its own errors
            fflush(stderr);
            _exit(1);
        }

    } else if (command == "help") {
        print_usage();

    } else {
        // FIX 22: Helpful error for -e/-c/--eval flags from other languages
        if (command == "-e" || command == "--eval" || command == "--exec" ||
            command == "-c" || command == "--command") {
            fmt::print(stderr,
                "Error: NAAb doesn't support inline code execution with '{}'.\n\n"
                "  Help: Write your code to a file and run it:\n"
                "    echo 'main {{ io.write(\"hello\") }}' > /tmp/eval.naab\n"
                "    naab-lang /tmp/eval.naab\n\n"
                "  Or use process substitution:\n"
                "    naab-lang <(echo 'main {{ io.write(\"hello\") }}')\n",
                command);
            fflush(stderr);
            _exit(1);
        }
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
