// The capability manifest's file format. See include/naab/capability_manifest.h.

#include "naab/capability_manifest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <set>

namespace naab {
namespace governance {

const char* const kCapabilityManifestLimitsNote =
    "Polyglot code is bounded by the sandbox only. A subprocess language "
    "(shell, node, ruby, php, go, ...) runs in its own process, where the "
    "hosts and files listed here do not reach (CONTRA-013); without "
    "SHELL_EXEC every separate-process language is refused. Effects inside "
    "polyglot blocks are not inferred: an embedded Python block is held to the "
    "files and hosts listed here, and a file or host it needs must be added "
    "by hand.";

std::string capabilityManifestPathFor(const std::string& program_path) {
    std::filesystem::path p(program_path);
    std::filesystem::path out = p.parent_path() / (p.stem().string() + ".manifest.json");
    return out.string();
}

namespace {

bool isLowerHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// An exact host name or IP literal: no wildcard, scheme, path, port,
// credentials or whitespace. Wildcards are refused because the manifest is
// intersected with the project's host lists entry by entry, which is exact
// only for exact names.
bool isExactHost(const std::string& h) {
    if (h.empty() || h.size() > 253) return false;
    for (char c : h) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':';
        if (!ok) return false;
    }
    return true;
}

bool readStringList(const nlohmann::json& j, const char* key, bool (*valid)(const std::string&),
                    std::vector<std::string>& out, std::string& error) {
    const auto& v = j.at(key);
    if (!v.is_array()) {
        error = std::string("\"") + key + "\" must be a list of strings";
        return false;
    }
    std::set<std::string> seen;
    for (const auto& e : v) {
        if (!e.is_string() || e.get<std::string>().empty()) {
            error = std::string("\"") + key + "\" must hold only non-empty strings";
            return false;
        }
        std::string s = e.get<std::string>();
        if (valid && !valid(s)) {
            error = std::string("\"") + key + "\" holds an entry the format does not allow: " + s;
            return false;
        }
        if (seen.insert(s).second) out.push_back(s);
    }
    return true;
}

}  // namespace

bool parseCapabilityManifest(const std::string& text,
                             const std::vector<std::string>& valid_actions,
                             CapabilityManifest& out, std::string& error) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        error = "the manifest is not valid JSON";
        return false;
    }
    if (!j.is_object()) {
        error = "the manifest must be a JSON object";
        return false;
    }
    static const std::set<std::string> kKeys = {
        "naab_manifest", "program", "modules", "actions", "languages",
        "hosts", "files", "agents"};
    for (const auto& [k, _] : j.items()) {
        if (!k.empty() && k[0] == '_') continue;
        if (!kKeys.count(k)) {
            error = "unknown key \"" + k + "\"";
            return false;
        }
    }
    for (const auto& k : kKeys) {
        if (!j.contains(k)) {
            error = "missing key \"" + k + "\"";
            return false;
        }
    }

    CapabilityManifest m;
    if (!j["naab_manifest"].is_number_integer() || j["naab_manifest"].get<int>() != 1) {
        error = "\"naab_manifest\" must be 1";
        return false;
    }

    const auto& prog = j["program"];
    if (!prog.is_object() || prog.size() != 2 || !prog.contains("file") ||
        !prog.contains("sha256") || !prog["file"].is_string() || !prog["sha256"].is_string()) {
        error = "\"program\" must be {\"file\": NAME, \"sha256\": HEX}";
        return false;
    }
    m.program_file = prog["file"].get<std::string>();
    m.program_sha256 = prog["sha256"].get<std::string>();
    if (m.program_file.empty() ||
        std::filesystem::path(m.program_file).filename().string() != m.program_file) {
        error = "\"program.file\" must be the program's file name, with no directory";
        return false;
    }
    if (!isLowerHex64(m.program_sha256)) {
        error = "\"program.sha256\" must be 64 lower-case hex digits";
        return false;
    }

    const auto& mods = j["modules"];
    if (!mods.is_object()) {
        error = "\"modules\" must be an object of path -> sha256";
        return false;
    }
    for (const auto& [path, hash] : mods.items()) {
        if (path.empty() || !hash.is_string() || !isLowerHex64(hash.get<std::string>())) {
            error = "\"modules\" entry \"" + path + "\" must map a path to 64 lower-case hex digits";
            return false;
        }
        m.modules[path] = hash.get<std::string>();
    }

    if (!readStringList(j, "actions", nullptr, m.actions, error)) return false;
    for (const auto& a : m.actions) {
        if (std::find(valid_actions.begin(), valid_actions.end(), a) == valid_actions.end()) {
            error = "\"actions\" holds an unknown action: " + a;
            return false;
        }
    }
    if (!readStringList(j, "languages", nullptr, m.languages, error)) return false;
    if (!readStringList(j, "hosts", isExactHost, m.hosts, error)) return false;
    for (auto& h : m.hosts) std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    if (!readStringList(j, "agents", nullptr, m.agents, error)) return false;

    const auto& files = j["files"];
    if (!files.is_object() || files.size() != 2 || !files.contains("read") ||
        !files.contains("write")) {
        error = "\"files\" must be {\"read\": [...], \"write\": [...]}";
        return false;
    }
    if (!readStringList(files, "read", nullptr, m.files_read, error)) return false;
    if (!readStringList(files, "write", nullptr, m.files_write, error)) return false;

    out = std::move(m);
    return true;
}

std::string serializeCapabilityManifest(const CapabilityManifest& m,
                                        const std::vector<std::string>& inference_notes) {
    auto sorted = [](std::vector<std::string> v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    };
    nlohmann::ordered_json j;
    j["naab_manifest"] = 1;
    j["program"] = {{"file", m.program_file}, {"sha256", m.program_sha256}};
    nlohmann::ordered_json mods = nlohmann::ordered_json::object();
    for (const auto& [p, h] : m.modules) mods[p] = h;
    j["modules"] = mods;
    j["actions"] = sorted(m.actions);
    j["languages"] = sorted(m.languages);
    j["hosts"] = sorted(m.hosts);
    j["files"] = {{"read", sorted(m.files_read)}, {"write", sorted(m.files_write)}};
    j["agents"] = sorted(m.agents);
    j["_limits"] = kCapabilityManifestLimitsNote;
    if (!inference_notes.empty()) j["_inference"] = inference_notes;
    return j.dump(2) + "\n";
}

}  // namespace governance
}  // namespace naab
