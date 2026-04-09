// naab-gov — Standalone Governance + Scanning CLI (Phase 8.2)
//
// Usage:
//   naab-gov lint <file.naab> [--config <govern.json>] [--sarif <out.sarif>]
//   naab-gov scan <path> [--language <lang>]
//   naab-gov --version
//   naab-gov --help
//
// Exit codes:
//   0  — no violations / clean scan
//   1  — I/O error or usage error
//   2  — quality gate failure
//   3  — HARD governance violation
//   4  — config error

#include "naab/governance.h"
#include "naab/scanner.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#define NAAB_GOV_VERSION "0.9.0"

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
        "  naab-gov scan <path> [options]         Scan code for quality issues\n"
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
        "scan OPTIONS:\n"
        "  --language <lang>       Force language detection (naab, python, go, ...)\n"
        "  --sarif                 Write SARIF to quality-report.sarif\n"
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
        if (!loaded) {
            std::cerr << "naab-gov lint: no govern.json found (starting from "
                      << dir << ")\n"
                         "  Run without --config to lint with default rules only, or\n"
                         "  create a govern.json at your project root.\n";
            // Proceed without governance — static pattern checks still run
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

    auto runCheck = [&](const std::string& result) {
        if (!result.empty()) errors.push_back(result);
    };

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
    runCheck(engine.checkCodeInjection("naab", source));
    runCheck(engine.checkHardcodedUrls(source));
    runCheck(engine.checkHardcodedIps(source));
    runCheck(engine.checkHardcodedResults(source));
    runCheck(engine.checkCustomRules("naab", source));

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
    if (naab::governance::g_governance_hard_block || engine.wasBlocked()) {
        return 3;
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
// scan command
// ---------------------------------------------------------------------------

static int cmdScan(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "naab-gov scan: no path specified\n"
                     "Usage: naab-gov scan <path> [--language <lang>] [--sarif]\n";
        return 1;
    }

    std::string target_path;
    std::string language;
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

    if (cmd == "lint") {
        return cmdLint(rest);
    }

    if (cmd == "scan") {
        return cmdScan(rest);
    }

    std::cerr << "naab-gov: unknown command: " << cmd << "\n"
                 "Run 'naab-gov --help' for usage.\n";
    return 1;
}
