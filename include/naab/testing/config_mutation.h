#pragma once
// ---------------------------------------------------------------------------
// TEST BUILD ONLY (cmake -DNAAB_CONFIG_MUTATION=ON). Compiled out of every
// normal build: in a shipped binary an environment variable able to delete a
// governance setting would be a bypass. tests/security/
// test_setting_drop_compiled_out.sh checks that on the build CI ships.
//
// NAAB_DROP_SETTING=a.b.c[,d.e] removes those settings from every config a
// loader that calls applySettingDrop() reads, so the reader falls back to its
// default. '*' matches any key at that level (agents.*.shell_allowed).
// tools/testrunner/setting_drop.py drops settings and asks whether any test
// that loads them notices: a setting no test notices is dead or untested.
//
// ONE copy, two callers, because govern.json has two readers:
//   - GovernanceEngine::loadFromJson (src/runtime/governance_config.cpp):
//     file, extends chain, inline string, mid-run reload;
//   - ScannerEngine::loadConfigFromPath (src/scanner/scanner.cpp), which
//     opens govern.json itself and reads its "scanner" section. Until this
//     header the hook lived in the first only, so a "removed" scanner setting
//     was still read by the scanner and every scanner setting read as
//     surviving (docs/settings-liveness.md, 2026-11-13).
// A new reader of govern.json must call it too, or its settings are
// unmeasurable by the tool.
//
// Every drop actually performed is appended to NAAB_DROP_LOG as one line,
// "<path><TAB><value as JSON>". A run that logs nothing never loaded the
// setting, and must be read as UNMEASURABLE, not as "the setting does nothing".
//
// This header includes nlohmann/json.hpp, against the project's rule for
// headers: it is included only by the two .cpp files above, which include
// json.hpp themselves, and only under #ifdef NAAB_CONFIG_MUTATION.
// ---------------------------------------------------------------------------
#ifndef NAAB_CONFIG_MUTATION
#error "naab/testing/config_mutation.h is for NAAB_CONFIG_MUTATION test builds only"
#endif

#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace naab { namespace testing {

inline void dropSettingAt(nlohmann::json& node, const std::vector<std::string>& parts,
                          size_t i, std::string& path, FILE* log) {
    if (!node.is_object() || i >= parts.size()) return;
    auto visit = [&](const std::string& key) {
        if (!node.contains(key)) return;
        size_t mark = path.size();
        path += (path.empty() ? "" : ".") + key;
        if (i + 1 == parts.size()) {
            if (log) fprintf(log, "%s\t%s\n", path.c_str(), node[key].dump().c_str());
            node.erase(key);
        } else {
            dropSettingAt(node[key], parts, i + 1, path, log);
        }
        path.resize(mark);
    };
    if (parts[i] == "*") {
        std::vector<std::string> keys;
        for (auto it = node.begin(); it != node.end(); ++it) keys.push_back(it.key());
        for (const auto& k : keys) visit(k);
    } else {
        visit(parts[i]);
    }
}

inline void logLoadedSettings(const nlohmann::json& node, std::string& path, FILE* log) {
    if (!node.is_object() || node.empty()) {
        if (!path.empty()) fprintf(log, "%s\n", path.c_str());
        return;
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        size_t mark = path.size();
        path += (path.empty() ? "" : ".") + it.key();
        logLoadedSettings(it.value(), path, log);
        path.resize(mark);
    }
}

inline nlohmann::json applySettingDrop(const nlohmann::json& j) {
    // NAAB_SETTINGS_LOG: every setting path this config contains, one per line
    // -- measured, so the harness knows which settings each test really loads.
    if (const char* p = std::getenv("NAAB_SETTINGS_LOG"); p && *p) {
        if (FILE* f = fopen(p, "a")) { std::string path; logLoadedSettings(j, path, f); fclose(f); }
    }
    const char* spec = std::getenv("NAAB_DROP_SETTING");
    if (!spec || !*spec) return j;
    nlohmann::json copy = j;
    const char* log_path = std::getenv("NAAB_DROP_LOG");
    FILE* log = (log_path && *log_path) ? fopen(log_path, "a") : nullptr;
    // Comma-separated: several settings may be dropped together, so a group
    // that shows no effect can be cleared in one run.
    std::stringstream all(spec);
    for (std::string one; std::getline(all, one, ',');) {
        if (one.empty()) continue;
        std::vector<std::string> parts;
        std::stringstream ss(one);
        for (std::string part; std::getline(ss, part, '.');) parts.push_back(part);
        std::string path;
        dropSettingAt(copy, parts, 0, path, log);
    }
    if (log) fclose(log);
    return copy;
}

} }  // namespace naab::testing
