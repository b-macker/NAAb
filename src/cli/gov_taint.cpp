// naab-gov taint FILE -- the static information-flow report (Bundle 12 PR 2).
//
// Reads FILE, loads the govern.json a run of it would find (or --config PATH),
// and reports every flow of tainted data to a configured sink that the static
// analysis (include/naab/taint_flow.h) finds, of two kinds:
//   value    the data reaching the sink is computed from a source: an
//            explicit flow, the kind a run's taint check exists to judge, under
//            the same names (sources, sanitizers, sinks);
//   control  the data is not, but it was set, or the sink was reached, only on
//            a path tainted data chose -- an implicit flow, which no run-time
//            check judges.
//
// Report only (the owner's decision, 2026-11-04): it refuses nothing and runs
// nothing. Its job is to measure how often these flows occur, and how often the
// analysis is wrong, before any of it is enforced.
//
// Exit: 0 no flow found and nothing left unanalysed, 1 one or more flows,
// 2 UNMEASURABLE (taint tracking is not enabled, no source or no sink is
// configured, or no flow was found but some constructs were not analysed -- a
// partial analysis that found nothing is not a clean one), 4 a usage, parse or
// config error.
#include "naab/governance.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/stdlib.h"
#include "naab/taint_flow.h"
#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const char* modeName(naab::governance::GovernanceMode m) {
    switch (m) {
        case naab::governance::GovernanceMode::ENFORCE: return "enforce";
        case naab::governance::GovernanceMode::AUDIT: return "audit";
        case naab::governance::GovernanceMode::OFF: return "off";
    }
    return "unknown";
}

// The report never reads its standard input, and the config load runs the
// config's `prerequisites` checks, which start the commands they name with this
// process's stdin (A122: a `command` check naming an interpreter reads -- and
// runs -- whatever the caller piped in, or waits for it). Point stdin at the
// null device before the load. The checks still run and still enforce; they
// only cannot read the caller's input.
void detachStdin() {
#ifdef _WIN32
    int fd = _open("NUL", _O_RDONLY);
    if (fd >= 0) { _dup2(fd, 0); if (fd != 0) _close(fd); }
#else
    int fd = ::open("/dev/null", O_RDONLY);
    if (fd >= 0) { ::dup2(fd, 0); if (fd != 0) ::close(fd); }
#endif
}

int unmeasurable(bool as_json, const std::string& file, const std::string& why) {
    if (as_json) {
        json j;
        j["file"] = file;
        j["result"] = "unmeasurable";
        j["reason"] = why;
        std::cout << j.dump(2) << "\n";
    } else {
        std::cout << "naab-gov taint: UNMEASURABLE -- " << why << "\n";
    }
    return 2;
}

}  // namespace

int cmdTaint(const std::vector<std::string>& args) {
    std::string file, config_path;
    bool as_json = false;
    auto usage = [](const std::string& why) {
        std::cerr << "naab-gov taint: " << why << "\n"
                     "Usage: naab-gov taint <file.naab> [--config <govern.json>] [--json]\n";
        return 4;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--config") {
            if (i + 1 >= args.size()) return usage("--config needs a path");
            config_path = args[++i];
        } else if (a == "--json") {
            as_json = true;
        } else if (!a.empty() && a[0] != '-' && file.empty()) {
            file = a;
        } else {
            return usage("unknown option: " + a);
        }
    }
    if (file.empty()) return usage("expected a .naab file");
    if (!fs::is_regular_file(file)) return usage("not a file: " + file);
    const fs::path abs = fs::absolute(file).lexically_normal();

    std::string source;
    {
        std::ifstream f(abs, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        source = ss.str();
    }
    std::unique_ptr<naab::ast::Program> program;
    try {
        naab::lexer::Lexer lex(source);
        auto tokens = lex.tokenize();
        naab::parser::Parser parser(tokens);
        parser.setSource(source, abs.string());
        program = parser.parseProgram();
    } catch (const std::exception& e) {
        std::cerr << "naab-gov taint: " << abs.string() << " does not parse: " << e.what() << "\n";
        return 4;
    }

    // The policy: the govern.json a run of this file would find, or the one named.
    detachStdin();
    naab::governance::GovernanceEngine engine;
    if (!config_path.empty()) {
        if (!fs::is_regular_file(config_path)) return usage("govern.json not found: " + config_path);
        if (!engine.loadFromFile(config_path)) {
            std::cerr << "naab-gov taint: govern.json does not load: " << engine.getLastError() << "\n";
            return 4;
        }
    } else if (!engine.discoverAndLoad(abs.parent_path().string())) {
        if (engine.getLastError() == "not_found")
            return unmeasurable(as_json, abs.string(),
                "no govern.json is found from " + abs.parent_path().string() +
                ", so no taint source or sink is configured");
        std::cerr << "naab-gov taint: govern.json does not load: " << engine.getLastError() << "\n";
        return 4;
    }
    const auto& tt = engine.getRules().taint_tracking;
    const std::string loaded = engine.getLoadedPath();
    if (!tt.enabled)
        return unmeasurable(as_json, abs.string(), "taint_tracking is not enabled in " + loaded);
    if (tt.sources.empty() || tt.sinks.empty())
        return unmeasurable(as_json, abs.string(),
            "taint_tracking in " + loaded + " lists no " + (tt.sources.empty() ? "source" : "sink"));

    naab::taint::FlowPolicy policy;
    policy.is_source = [&engine](const std::string& n) { return engine.isTaintSource(n); };
    policy.is_sanitizer = [&engine](const std::string& n) { return engine.isSanitizer(n); };
    policy.is_sink = [&engine](const std::string& n) { return engine.isTaintSink(n); };
    naab::stdlib::StdLib stdlib_modules;
    policy.is_stdlib_module = [&stdlib_modules](const std::string& n) { return stdlib_modules.hasModule(n); };
    const naab::taint::FlowReport report = naab::taint::analyzeProgram(*program, policy);

    int n_value = 0, n_control = 0, n_blind = 0;
    for (const auto& f : report.findings)
        (f.kind == naab::taint::FlowFinding::Kind::Value ? n_value : n_control)++;
    for (const auto& [what, n] : report.blind_spots) n_blind += n;
    const bool any = !report.findings.empty();
    const char* result = any ? "flows" : (n_blind > 0 ? "unmeasurable" : "none");

    if (as_json) {
        json j;
        j["file"] = abs.string();
        j["config"] = loaded;
        j["mode"] = modeName(engine.getMode());
        j["result"] = result;
        j["functions_analysed"] = report.functions_analysed;
        j["converged"] = report.converged;
        j["flows"] = json::array();
        for (const auto& f : report.findings)
            j["flows"].push_back({{"kind", naab::taint::kindName(f.kind)}, {"sink", f.sink},
                                  {"line", f.line}, {"function", f.function},
                                  {"origin", f.origin}, {"via", f.via}});
        j["not_analysed"] = json::object();
        for (const auto& [what, n] : report.blind_spots) j["not_analysed"][what] = n;
        std::cout << j.dump(2) << "\n";
    } else {
        std::cout << "naab-gov taint: " << abs.string() << "\n"
                  << "  config: " << loaded << " (mode " << modeName(engine.getMode()) << "; "
                  << tt.sources.size() << " sources, " << tt.sinks.size() << " sinks, "
                  << tt.sanitizers.size() << " sanitizers)\n";
        for (const auto& f : report.findings) {
            std::cout << "  " << (f.kind == naab::taint::FlowFinding::Kind::Value ? "value  " : "control")
                      << " line " << f.line << ": " << f.sink << " in " << f.function;
            if (!f.via.empty()) std::cout << " (" << f.via << ")";
            std::cout << "\n          from " << f.origin << "\n";
        }
        for (const auto& [what, n] : report.blind_spots)
            std::cout << "  not analysed (" << n << "): " << what << "\n";
        std::cout << "  result: ";
        if (any)
            std::cout << report.findings.size() << " flow(s): " << n_value << " value, " << n_control
                      << " control";
        else if (n_blind > 0)
            std::cout << "UNMEASURABLE -- no flow found in what was analysed, but " << n_blind
                      << " construct(s) were not";
        else
            std::cout << "no flow found";
        std::cout << " (" << report.functions_analysed << " function(s) analysed)\n";
        if (any)
            std::cout << "  value: the data is computed from a source (what a run's taint check judges); "
                         "control: only the path is (no run-time check judges it).\n";
    }
    if (any) return 1;
    return n_blind > 0 ? 2 : 0;
}
