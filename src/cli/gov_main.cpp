// naab-gov — Standalone Governance + Scanning CLI (Phase 8.2)
//
// Usage:
//   naab-gov lint <file.naab> [--config <govern.json>] [--sarif <out.sarif>]
//   naab-gov check --language <lang> [options]    (reads from stdin or --file)
//   naab-gov scan <path> [--language <lang>]
//   naab-gov languages                            (the language table, JSON)
//   naab-gov ratchet <old.json> <new.json>        (would a reload accept NEW?)
//   naab-gov contracts <file.naab>                (counterexample search; RUNS code)
//   naab-gov --version
//   naab-gov --help
//
// Exit codes:
//   0  — no violations / clean scan
//   1  — I/O error or usage error
//   2  — quality gate failure
//   3  — HARD governance violation
//   4  — config error

#include "naab/compiler.h"
#include "naab/governance.h"
#include "naab/language_descriptors.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/scanner.h"
#include "naab/process_exit.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define STDIN_FILENO _fileno(stdin)
#else
#include <unistd.h>
#endif

// One version for the whole tree: include/naab/config.h (scripts/bump-version.sh).
#include "naab/config.h"
#define NAAB_GOV_VERSION NAAB_VERSION_STRING

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void printHelp() {
    std::cout <<
        "naab-gov " NAAB_GOV_VERSION " — Standalone NAAb Governance + Scanning CLI\n"
        "\n"
        "USAGE:\n"
        "  naab-gov lint <file.naab> [options]   Lint a NAAb file (static governance checks)\n"
        "  naab-gov check --language <lang> [options]  Check code from stdin or file\n"
        "  naab-gov scan <path> [options]         Scan code for quality issues\n"
        "  naab-gov languages                     Print the language table (JSON)\n"
        "  naab-gov ratchet <old> <new> [options] Would a mid-run reload of <old> by <new>\n"
        "                                         be accepted? (the reload ratchet)\n"
        "  naab-gov contracts <file.naab> [opts]  Search for inputs that meet a function's\n"
        "                                         requires and break a clause (RUNS the\n"
        "                                         functions, in a naab-lang worker)\n"
        "  naab-gov --version                     Print version and exit\n"
        "  naab-gov --help                        Print this help and exit\n"
        "\n"
        "lint OPTIONS:\n"
        "  --config <govern.json>  Use this govern.json instead of auto-discovery\n"
        "  --config-from-file      Discover govern.json from the scanned file's directory\n"
        "                          (default: discover from CWD — safer for untrusted files)\n"
        "  --sarif <file>          Write findings as SARIF to <file>\n"
        "  --junit <file>          Write findings as JUnit XML to <file>\n"
        "  --env <name>            Apply named environment overlay from govern.json\n"
        "\n"
        "check OPTIONS:\n"
        "  --language <lang>       Target language (required: python, javascript, go, ...)\n"
        "  --file <path>           Read code from file instead of stdin\n"
        "  --config <govern.json>  Use this govern.json instead of auto-discovery\n"
        "  --config-string <json>  Use inline JSON config string\n"
        "  --sarif                 Output SARIF instead of JSON\n"
        "  --env <name>            Apply named environment overlay from govern.json\n"
        "\n"
        "scan OPTIONS:\n"
        "  --language <lang>       Force language detection (naab, python, go, ...)\n"
        "  --sarif                 Write SARIF to quality-report.sarif\n"
        "\n"
        "ratchet OPTIONS:\n"
        "  --env <name>            Start under this environment overlay, as naab-lang --env\n"
        "                          (default: govern.json's governance.default_env)\n"
        "  --agent-id <name>       Start under this agent role, as naab-lang --agent-id\n"
        "  --lease-grant <a,b,..>  Start under this capability lease (FS_READ, FS_WRITE,\n"
        "                          NET_CONNECT, SHELL_EXEC, ENV_READ, ENV_WRITE, AGENT_SEND)\n"
        "  --ratchet-only          Judge the ratchet alone; a reload also verifies <new>'s\n"
        "                          signature, which is checked unless this is given\n"
        "  --json                  Print the decision as JSON\n"
        "  <new> is judged as if it replaced <old> in place: its relative paths resolve\n"
        "  against <old>'s directory. Exit 0 = accepted, 1 = refused, 4 = a config error.\n"
        "\n"
        "contracts OPTIONS:\n"
        "  --seed <n>              Seed for the generated inputs (default 1; same seed,\n"
        "                          same inputs)\n"
        "  --cases <n>             Inputs tried per function (default 200)\n"
        "  --timeout <s>           Time limit for the worker (default 60; govern.json's\n"
        "                          limit caps it)\n"
        "  --naab <path>           The naab-lang to run (default: beside naab-gov)\n"
        "  --json                  Print the report as JSON\n"
        "  Exit 0 = no counterexample found, 1 = a counterexample, 2 = UNMEASURABLE\n"
        "  (nothing tested), 3 = a governance block, 4 = a config or usage error.\n"
        "\n"
        "EXIT CODES:\n"
        "  0  Clean — no violations\n"
        "  1  I/O or usage error\n"
        "  2  Quality gate failure\n"
        "  3  HARD governance block\n"
        "  4  Configuration error\n"
    ;
}

// ---------------------------------------------------------------------------
// lint command
// ---------------------------------------------------------------------------

static int cmdLint(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "naab-gov lint: no file specified\n"
                     "Usage: naab-gov lint <file.naab> [--config govern.json] [--sarif out.sarif]\n";
        return 1;
    }

    std::string source_path;
    std::string config_path;
    std::string sarif_path;
    std::string junit_path;
    std::string env_name;
    bool config_from_file = false; // V-GOV-009: opt-in to source-relative discovery

    // Parse args
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--config" && i + 1 < args.size()) {
            config_path = args[++i];
        } else if (args[i] == "--sarif" && i + 1 < args.size()) {
            sarif_path = args[++i];
        } else if (args[i] == "--junit" && i + 1 < args.size()) {
            junit_path = args[++i];
        } else if (args[i] == "--env" && i + 1 < args.size()) {
            env_name = args[++i];
        } else if (args[i] == "--config-from-file") {
            config_from_file = true;
        } else if (args[i][0] != '-') {
            source_path = args[i];
        } else {
            std::cerr << "naab-gov lint: unknown option: " << args[i] << "\n";
            return 4;
        }
    }

    if (source_path.empty()) {
        std::cerr << "naab-gov lint: no source file specified\n";
        return 1;
    }

    if (!fs::exists(source_path)) {
        std::cerr << "naab-gov lint: file not found: " << source_path << "\n";
        return 1;
    }

    std::string source;
    try {
        source = readFile(source_path);
    } catch (const std::exception& e) {
        std::cerr << "naab-gov lint: " << e.what() << "\n";
        return 1;
    }

    // Load governance
    naab::governance::GovernanceEngine engine;

    bool loaded = false;
    if (!config_path.empty()) {
        if (!fs::exists(config_path)) {
            std::cerr << "naab-gov lint: govern.json not found: " << config_path << "\n";
            return 4;
        }
        loaded = engine.loadFromFile(config_path);
        if (!loaded) {
            std::cerr << "naab-gov lint: failed to parse: " << config_path << "\n";
            return 4;
        }
    } else {
        // V-GOV-009: discover from CWD (caller's project root) by default.
        // An attacker-supplied file sitting next to its own govern.json cannot
        // override the scanner's governance policy this way.
        // Use --config-from-file to opt back into source-relative discovery
        // (useful in monorepos where each subdirectory owns its govern.json).
        std::string dir = config_from_file
            ? fs::path(fs::absolute(source_path)).parent_path().string()
            : fs::current_path().string();
        loaded = engine.discoverAndLoad(dir);
        if (!loaded && engine.getLastError() != "not_found") {
            std::cerr << "naab-gov lint: govern.json was found but could not be loaded\n";
            return 4;
        }
        if (!loaded) {
            std::cerr << "naab-gov lint: no governance config found (govern.json or --config required)\n"
                         "  Searched from: " << dir << "\n";
            return 4;
        }
    }

    // Apply environment overlay
    if (!env_name.empty() && loaded) {
        engine.applyEnvironment(env_name);
    }

    // Set check context to the source file
    engine.setCheckContext(source_path, 0);

    // Run static governance checks on the NAAb source
    // These check methods work purely on source strings — no execution needed.
    std::vector<std::string> errors;
    bool hard_blocked = false;
    bool not_checked = false;  // could not be checked in full (parse/compile failure)

    auto runCheck = [&](const std::string& result) {
        if (!result.empty()) errors.push_back(result);
    };

    // A HARD finding throws GovernanceHardError. It is caught here rather than
    // in main() so the SARIF/JUnit reports below are still written: CI lints
    // module files and uploads those reports, and a report that goes missing
    // exactly when a file is blocked drops the finding it was written for.
    try {
        runCheck(engine.checkBannedFunctions("naab", source));
        runCheck(engine.checkPlaceholders(source));
        runCheck(engine.checkSecrets(source));
        runCheck(engine.checkTemporaryCode(source));
        runCheck(engine.checkSimulationMarkers(source));
        runCheck(engine.checkMockData(source));
        runCheck(engine.checkApologeticLanguage(source));
        runCheck(engine.checkDeadCode(source));
        runCheck(engine.checkDebugArtifacts("naab", source));
        runCheck(engine.checkUnsafeDeserialization(source));
        runCheck(engine.checkSqlInjection(source));
        runCheck(engine.checkPathTraversal(source));
        runCheck(engine.checkShellInjection(source));
        runCheck(engine.checkCodeInjection("naab", source, source));
        runCheck(engine.checkHardcodedUrls(source));
        runCheck(engine.checkHardcodedIps(source));
        runCheck(engine.checkHardcodedResults(source));
        runCheck(engine.checkCustomRules("naab", source));

        // What a run checks before it executes anything, and lint used to skip:
        // the whole-source PII and incomplete-logic checks and the intent
        // preflight (main.cpp, VM path), and the per-function checks -- contracts,
        // complexity floor, oversimplification, ... -- which the VM compiler
        // applies to every function body it compiles. Compiling here applies
        // exactly those, through the compiler's own body extraction, and executes
        // nothing. Without this a module linted in CI got fewer checks than the
        // same file run, which matters because a module cannot be run under
        // requirements.main_block.
        runCheck(engine.checkPii(source));
        runCheck(engine.checkIncompleteLogic(source, 0, source_path));

        std::unique_ptr<naab::ast::Program> program;
        try {
            naab::lexer::Lexer lexer(source);
            auto tokens = lexer.tokenize();
            naab::parser::Parser parser(tokens);
            parser.setSource(source, source_path);
            program = parser.parseProgram();
        } catch (const naab::governance::GovernanceHardError&) {
            throw;
        } catch (const std::exception& e) {
            // A file that does not parse cannot be checked function by function,
            // so it is not clean whatever the source-text checks said.
            std::cerr << "naab-gov lint: cannot parse " << source_path << ": " << e.what() << "\n";
            not_checked = true;
        }
        if (program) {
            runCheck(engine.preflightIntentCheck(*program, source));
            naab::vm::Compiler compiler;
            compiler.setGovernance(&engine);
            compiler.setSourceText(source);
            compiler.compile(*program, source_path);
        }
    } catch (const naab::governance::GovernanceHardError& e) {
        std::cerr << e.what() << "\n";
        hard_blocked = true;
    } catch (const std::exception& e) {
        // A DETECT-level finding reaches here as the compiler's exception, and
        // the engine has recorded it (wasBlocked() below). Anything else is a
        // compile failure: the functions after it were never checked.
        std::cerr << "naab-gov lint: " << e.what() << "\n";
        errors.push_back(e.what());
        not_checked = true;
    }

    // Print summary to stderr
    std::string summary = engine.formatSummary();
    if (!summary.empty()) {
        std::cerr << summary << "\n";
    }

    // Write SARIF if requested
    if (!sarif_path.empty()) {
        std::string sarif = engine.generateSarifReport();
        std::ofstream sf(sarif_path);
        if (!sf) {
            std::cerr << "naab-gov lint: cannot write SARIF to: " << sarif_path << "\n";
            return 1;
        }
        sf << sarif;
        std::cerr << "naab-gov lint: SARIF written to " << sarif_path << "\n";
    }

    // Write JUnit if requested
    if (!junit_path.empty()) {
        std::string junit = engine.generateJunitReport();
        std::ofstream jf(junit_path);
        if (!jf) {
            std::cerr << "naab-gov lint: cannot write JUnit to: " << junit_path << "\n";
            return 1;
        }
        jf << junit;
        std::cerr << "naab-gov lint: JUnit written to " << junit_path << "\n";
    }

    // Determine exit code
    if (hard_blocked || naab::governance::g_governance_hard_block || engine.wasBlocked()) {
        return 3;
    }
    if (not_checked) {
        return 1;
    }
    std::string qg = engine.evaluateQualityGate();
    if (!qg.empty()) {
        std::cerr << "naab-gov lint: quality gate failed: " << qg << "\n";
        return 2;
    }
    if (!errors.empty()) {
        // Errors from SOFT/ADVISORY checks — still exit 0 (they were advisory/soft)
        // HARD blocks would have been caught by wasBlocked()
        return 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// check command — polyglot governance via stdin or file
// ---------------------------------------------------------------------------

static int cmdCheck(const std::vector<std::string>& args) {
    std::string language;
    std::string file_path;
    std::string config_path;
    std::string config_string;
    std::string env_name;
    bool sarif_output = false;

    // Parse args
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--language" && i + 1 < args.size()) {
            language = args[++i];
        } else if (args[i] == "--file" && i + 1 < args.size()) {
            file_path = args[++i];
        } else if (args[i] == "--config" && i + 1 < args.size()) {
            config_path = args[++i];
        } else if (args[i] == "--config-string" && i + 1 < args.size()) {
            config_string = args[++i];
        } else if (args[i] == "--env" && i + 1 < args.size()) {
            env_name = args[++i];
        } else if (args[i] == "--sarif") {
            sarif_output = true;
        } else {
            std::cerr << "naab-gov check: unknown option: " << args[i] << "\n";
            return 1;
        }
    }

    if (!config_string.empty() && !config_path.empty()) {
        std::cerr << "naab-gov check: --config and --config-string are mutually exclusive\n";
        return 4;
    }

    if (language.empty()) {
        std::cerr << "naab-gov check: --language is required\n"
                     "Usage: naab-gov check --language <lang> [--file <path>] [--config govern.json]\n";
        return 1;
    }

    // Read code from --file or stdin
    std::string code;
    std::string source_file = "stdin";
    if (!file_path.empty()) {
        if (!fs::exists(file_path)) {
            std::cerr << "naab-gov check: file not found: " << file_path << "\n";
            return 1;
        }
        try {
            code = readFile(file_path);
            source_file = file_path;
        } catch (const std::exception& e) {
            std::cerr << "naab-gov check: " << e.what() << "\n";
            return 1;
        }
    } else {
        if (isatty(STDIN_FILENO)) {
            std::cerr << "naab-gov check: reading from stdin (press Ctrl+D when done, or use --file)\n";
        }
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        code = ss.str();
    }

    if (code.empty()) {
        std::cerr << "naab-gov check: no input (pipe code to stdin or use --file)\n";
        return 1;
    }

    // Load governance config
    //
    // Threat model for --config-string: naab-gov is a user-space CLI tool,
    // never setuid/setgid, never runs with elevated privileges. The caller
    // already has full control of the process (can pass --no-governance to
    // naab-lang, or simply not invoke governance at all). --config-string
    // exists for programmatic callers (Python subprocess binding) that need
    // to pass config without writing temp files that require Ed25519 signing.
    // Signature verification gates file-based configs; inline configs are
    // trusted as caller-provided input, same as any other CLI argument.
    naab::governance::GovernanceEngine engine;

    bool loaded = false;
    if (!config_string.empty()) {
        loaded = engine.loadFromString(config_string);
        if (!loaded) {
            std::cerr << "naab-gov check: failed to parse config string\n";
            return 4;
        }
    } else if (!config_path.empty()) {
        if (!fs::exists(config_path)) {
            std::cerr << "naab-gov check: govern.json not found: " << config_path << "\n";
            return 4;
        }
        loaded = engine.loadFromFile(config_path);
        if (!loaded) {
            std::cerr << "naab-gov check: failed to parse: " << config_path << "\n";
            return 4;
        }
    } else {
        loaded = engine.discoverAndLoad(fs::current_path().string());
        // No config is fine — pattern checks still run. A config that was
        // FOUND and did not load is not: a verdict reached without the
        // project's policy would read as the project's (A84).
        if (!loaded && engine.getLastError() != "not_found") {
            std::cerr << "naab-gov check: govern.json was found but could not be loaded\n";
            return 4;
        }
    }

    if (!env_name.empty() && loaded) {
        engine.applyEnvironment(env_name);
    }

    // Run polyglot governance checks.
    //
    // checkPolyglotBlock() THROWS GovernanceHardError on a HARD block. Letting
    // that escape sends it to main()'s handler, which prints e.what() as plain
    // text and _exit(3)s -- so the structured output below was reachable only
    // on a PASS. That made `check`'s contract asymmetric: JSON when nothing is
    // wrong, unformatted prose exactly when something is. A caller parsing
    // stdout for `"blocked": true` therefore never saw a block; the deleted
    // tests/security/test_alias_bypass.sh did precisely that and reported 86
    // false failures. The exit code was always correct (3), so this is an
    // output-shape defect, not a fail-open one.
    //
    // Catching it here is the point, not a leak: this is the CLI boundary
    // where a verdict becomes a report, the same role main()'s handler plays.
    // The engine has already recorded the violation, so the block is rendered
    // from getCheckResults() like any other, and the exit code below is
    // unchanged.
    std::string hard_block_message;
    try {
        engine.setCheckContext(source_file, 1);
        engine.checkPolyglotSource(language, code, source_file, 1);
    } catch (const naab::governance::GovernanceHardError& e) {
        hard_block_message = e.what();
    }

    // Output
    if (sarif_output) {
        std::cout << engine.generateSarifReport() << "\n";
    } else {
        // Build structured JSON output
        using json = nlohmann::json;
        json result;
        // A thrown HARD block counts as blocked whether or not wasBlocked()
        // reflects it, so the field can never read false on a refused input.
        result["blocked"] = engine.wasBlocked() || !hard_block_message.empty();

        json violations = json::array();
        for (const auto& cr : engine.getCheckResults()) {
            if (!cr.passed) {
                json v;
                v["rule"] = cr.rule_name;
                v["level"] = static_cast<int>(cr.level);
                v["message"] = cr.message;
                v["severity"] = cr.severity;
                if (!cr.cwe_ids.empty()) v["cwe_ids"] = cr.cwe_ids;
                if (!cr.owasp_ids.empty()) v["owasp_ids"] = cr.owasp_ids;
                violations.push_back(std::move(v));
            }
        }
        result["violations"] = violations;
        result["violation_count"] = violations.size();

        // The engine's own prose for a HARD block. Preserved so the fix loses
        // no detail that used to reach the terminal -- it moves into the
        // document rather than being dropped. Absent on a pass.
        if (!hard_block_message.empty()) {
            result["hard_block"] = hard_block_message;
        }

        std::cout << result.dump(2) << "\n";
    }

    // Exit code — unchanged: 3 on a block, and a thrown HARD block is one.
    if (naab::governance::g_governance_hard_block || engine.wasBlocked() ||
        !hard_block_message.empty()) {
        return 3;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// scan command
// ---------------------------------------------------------------------------

static int cmdScan(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "naab-gov scan: no path specified\n"
                     "Usage: naab-gov scan <path> [--language <lang>] [--sarif]\n";
        return 1;
    }

    std::string target_path;
    std::string language = "auto";
    bool sarif_output = false;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--language" && i + 1 < args.size()) {
            language = args[++i];
        } else if (args[i] == "--sarif") {
            sarif_output = true;
        } else if (args[i][0] != '-') {
            target_path = args[i];
        } else {
            std::cerr << "naab-gov scan: unknown option: " << args[i] << "\n";
            return 4;
        }
    }

    if (target_path.empty()) {
        std::cerr << "naab-gov scan: no target path specified\n";
        return 1;
    }

    if (!fs::exists(target_path)) {
        std::cerr << "naab-gov scan: path not found: " << target_path << "\n";
        return 1;
    }

    naab::scanner::ScannerEngine scanner;

    // V-GOV-009: discover governance config from CWD, not the target path
    scanner.loadConfig(fs::current_path().string());

    // Run scan
    naab::scanner::ScanResult result = scanner.scan(target_path, language);

    // Output
    if (sarif_output) {
        std::cout << scanner.formatSarifReport(result);
        scanner.saveReports(result);
    } else {
        std::cout << scanner.formatTextReport(result);
        scanner.saveReports(result);
    }

    std::cerr << "naab-gov scan: " << result.files_scanned << " file(s) scanned, "
              << result.issues.size() << " issue(s) found\n";

    return result.issues.empty() ? 0 : 2;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

// The language table (naab/language_descriptors.h) as JSON: what governance
// knows about each language. tests/self-audit/test_langconform.sh generates
// its probes from this, so a language added to the table is covered by the
// conformance matrix without anyone editing the test.
static int cmdLanguages() {
    using json = nlohmann::json;
    json out = json::array();
    for (const auto& d : naab::lang::allLanguages()) {
        json interps = json::array();
        for (const auto& in : d.interpolations) {
            interps.push_back({{"quote", in.quote}, {"prefix", in.prefix},
                               {"open", in.open}, {"close", in.close}});
        }
        json blocks = json::array();
        for (const auto& b : d.block_comments) {
            blocks.push_back({{"open", b.open}, {"close", b.close},
                              {"line_start_only", b.line_start_only}});
        }
        // Emit line-comment markers as plain strings (langconform reads them as
        // such); the context flags that govern WHERE each begins are verified
        // against the real interpreter, not from this JSON.
        json line_comments = json::array();
        for (const auto& m : d.line_comments) line_comments.push_back(m.marker);
        out.push_back({{"canonical", d.canonical}, {"aliases", d.aliases},
                       {"line_comments", line_comments}, {"block_comments", blocks},
                       {"governance_only", d.governance_only},
                       {"extension", d.extension}, {"prelude", d.prelude},
                       {"syntax_checks", d.syntax_checks},
                       {"runtime_variants", d.runtime_variants},
                       {"interpolations", interps},
                       {"backtick_runs_command", d.backtick_runs_command}});
    }
    std::cout << out.dump(1) << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// ratchet command (Bundle 8)
// ---------------------------------------------------------------------------
//
// Asks the engine what a mid-run reload of <old> by <new> would decide. The
// decision is GovernanceEngine::evaluateReload(), the function reloadIfChanged()
// itself calls, so this answers for the running engine rather than for a copy
// of its rules: <old> is loaded as `naab-lang run` loads a config (the lease
// before the load, the role and the environment after it, in that order), and
// <new> is prepared as a reload prepares the changed file.
// gov_contracts.cpp (Bundle 11)
int cmdContracts(const std::vector<std::string>& args, const char* argv0);

static int cmdRatchet(const std::vector<std::string>& args) {
    using json = nlohmann::json;
    std::vector<std::string> files;
    std::string env_name, agent_id, lease;
    bool lease_set = false, ratchet_only = false, as_json = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--env" && i + 1 < args.size()) {
            env_name = args[++i];
        } else if (a == "--agent-id" && i + 1 < args.size()) {
            agent_id = args[++i];
        } else if (a == "--lease-grant" && i + 1 < args.size()) {
            lease = args[++i];
            lease_set = true;
        } else if (a == "--ratchet-only") {
            ratchet_only = true;
        } else if (a == "--json") {
            as_json = true;
        } else if (!a.empty() && a[0] != '-') {
            files.push_back(a);
        } else {
            std::cerr << "naab-gov ratchet: unknown option: " << a << "\n";
            return 4;
        }
    }
    if (files.size() != 2) {
        std::cerr << "naab-gov ratchet: expected two files\n"
                     "Usage: naab-gov ratchet <old govern.json> <new govern.json> [options]\n";
        return 4;
    }
    for (const auto& f : files) {
        if (!fs::is_regular_file(f)) {
            std::cerr << "naab-gov ratchet: not a file: " << f << "\n";
            return 4;
        }
    }
    // Absolute, as a run's discovered govern.json is: the loaded file's
    // directory is where relative policy entries resolve and what the
    // self-protected paths name.
    const std::string old_path = fs::absolute(files[0]).lexically_normal().string();
    const std::string new_path = fs::absolute(files[1]).lexically_normal().string();

    using Engine = naab::governance::GovernanceEngine;
    if (lease_set) {
        const auto& known = Engine::leaseActions();
        std::vector<std::string> actions;
        std::stringstream ss(lease);
        std::string act;
        while (std::getline(ss, act, ',')) {
            if (act.empty()) continue;
            if (std::find(known.begin(), known.end(), act) == known.end()) {
                std::cerr << "naab-gov ratchet: unknown lease action: " << act << "\n";
                return 4;
            }
            actions.push_back(act);
        }
        Engine::setLeaseGrant(actions);
    }

    Engine engine;
    // As `naab-lang run` loads the govern.json it discovers (discoverAndLoad):
    // the file, then the self-protected paths and the project context.
    if (!engine.loadProjectConfig(old_path, fs::path(old_path).parent_path().string())) {
        std::cerr << "naab-gov ratchet: cannot load " << files[0] << "\n";
        return 4;
    }
    // As main.cpp: the agent id is the flag, else "anonymous", and an
    // "anonymous" one takes govern.json's agent_id; the environment is the
    // flag, else governance.default_env.
    if (agent_id.empty()) agent_id = "anonymous";
    if (!engine.getRules().agent_id_config.empty() && agent_id == "anonymous")
        agent_id = engine.getRules().agent_id_config;
    engine.setAgentId(agent_id);
    engine.applyAgentRole();
    if (env_name.empty()) env_name = engine.getRules().default_env;
    if (!env_name.empty()) {
        if (!engine.getRules().environments.count(env_name)) {
            std::cerr << "naab-gov ratchet: environment '" << env_name
                      << "' is not in " << files[0] << "\n";
            return 4;
        }
        engine.applyEnvironment(env_name);
    }

    using Outcome = Engine::ReloadEvaluation::Outcome;
    Engine::ReloadEvaluation ev;
    try {
        ev = engine.evaluateReload(new_path, !ratchet_only);
    } catch (const std::exception& e) {
        std::cerr << "naab-gov ratchet: cannot read " << files[1] << ": " << e.what() << "\n";
        return 4;
    }

    const char* outcome = "accepted";
    int rc = 0;
    switch (ev.outcome) {
        case Outcome::Accepted:         outcome = "accepted"; rc = 0; break;
        case Outcome::RatchetRefused:   outcome = "ratchet_refused"; rc = 1; break;
        case Outcome::SignatureRefused: outcome = "signature_refused"; rc = 1; break;
        case Outcome::Unreadable:       outcome = "unreadable"; rc = 4; break;
        case Outcome::TooWide:          outcome = "too_wide"; rc = 4; break;
    }
    if (as_json) {
        json out = {
            {"outcome", outcome},
            {"violations", ev.violations},
            {"notices", ev.notices},
            {"signature_checked", ev.signature_checked},
        };
        if (ev.outcome == Outcome::SignatureRefused) out["signature_reason"] = ev.signature_diag;
        std::cout << out.dump(1) << "\n";
        return rc;
    }
    switch (ev.outcome) {
        case Outcome::Accepted:
            std::cout << "ACCEPTED: a reload of " << files[0] << " by " << files[1]
                      << " would be installed\n";
            break;
        case Outcome::RatchetRefused:
            std::cout << "REFUSED: the reload would loosen governance\n";
            for (const auto& v : ev.violations) std::cout << "  violation: " << v << "\n";
            break;
        case Outcome::SignatureRefused:
            std::cout << "REFUSED: " << files[1] << "'s signature does not verify\n";
            if (!ev.signature_diag.empty()) std::cout << ev.signature_diag;
            break;
        case Outcome::Unreadable:
            std::cout << "ERROR: cannot open " << files[1] << "\n";
            break;
        case Outcome::TooWide:
            std::cout << "ERROR: a JSON array in " << files[1] << " exceeds the loader's cap\n";
            break;
    }
    for (const auto& n : ev.notices) std::cout << "  notice: " << n << "\n";
    if (ev.outcome == Outcome::Accepted || ev.outcome == Outcome::RatchetRefused) {
        std::cout << (ev.signature_checked
                          ? "  signature: verified as a reload would\n"
                          : "  signature: not checked (--ratchet-only)\n");
    }
    return rc;
}


int main(int argc, char* argv[]) {
    if (argc < 2) {
        printHelp();
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "--version" || cmd == "-v") {
        std::cout << "naab-gov " NAAB_GOV_VERSION "\n";
        return 0;
    }

    if (cmd == "--help" || cmd == "-h") {
        printHelp();
        return 0;
    }

    // Build remaining args
    std::vector<std::string> rest;
    for (int i = 2; i < argc; ++i) {
        rest.emplace_back(argv[i]);
    }

    try {
        if (cmd == "lint") {
            return cmdLint(rest);
        }

        if (cmd == "check") {
            return cmdCheck(rest);
        }

        if (cmd == "scan") {
            return cmdScan(rest);
        }

        if (cmd == "languages") {
            return cmdLanguages();
        }

        if (cmd == "ratchet") {
            return cmdRatchet(rest);
        }

        if (cmd == "contracts") {
            return cmdContracts(rest, argv[0]);
        }
    } catch (const naab::governance::GovernanceHardError& e) {
        std::cerr << e.what() << "\n";
        naab::exitProcess(3);
    }

    std::cerr << "naab-gov: unknown command: " << cmd << "\n"
                 "Run 'naab-gov --help' for usage.\n";
    return 1;
}
