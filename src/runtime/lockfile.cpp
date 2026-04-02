//
// NAAb Lockfile — naab.lock implementation
// Records and verifies observed polyglot runtime versions.
//

#include "naab/lockfile.h"
#include "naab/config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <chrono>
#include <ctime>

namespace naab {

// ============================================================================
// Helpers
// ============================================================================

static std::string currentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return buf;
}

static std::string detectPlatform() {
    std::string os = "linux";
    std::string arch = "unknown";
#if defined(__APPLE__)
    os = "darwin";
#elif defined(_WIN32)
    os = "windows";
#endif
#if defined(__aarch64__)
    arch = "arm64";
#elif defined(__x86_64__)
    arch = "x86_64";
#elif defined(__arm__)
    arch = "arm";
#endif
    return os + "/" + arch;
}

// ============================================================================
// Lockfile::load
// ============================================================================

Lockfile Lockfile::load(const std::string& path) {
    Lockfile lf;
    lf.naab_version = NAAB_VERSION_STRING;
    lf.platform = detectPlatform();

    std::ifstream ifs(path);
    if (!ifs.is_open()) return lf;

    try {
        nlohmann::json j = nlohmann::json::parse(ifs);
        if (j.contains("naab_version") && j["naab_version"].is_string())
            lf.naab_version = j["naab_version"].get<std::string>();
        if (j.contains("platform") && j["platform"].is_string())
            lf.platform = j["platform"].get<std::string>();
        if (j.contains("runtimes") && j["runtimes"].is_array()) {
            for (const auto& entry : j["runtimes"]) {
                LockfileEntry e;
                if (entry.contains("language"))
                    e.language = entry["language"].get<std::string>();
                if (entry.contains("runtime_version"))
                    e.runtime_version = entry["runtime_version"].get<std::string>();
                if (entry.contains("binary_path"))
                    e.binary_path = entry["binary_path"].get<std::string>();
                if (entry.contains("timestamp"))
                    e.timestamp = entry["timestamp"].get<std::string>();
                if (!e.language.empty())
                    lf.runtimes.push_back(e);
            }
        }
    } catch (...) {
        // Malformed lockfile — return with only the data we could parse
    }
    return lf;
}

// ============================================================================
// Lockfile::save
// ============================================================================

void Lockfile::save(const std::string& path) const {
    // Create parent directory if needed
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }

    nlohmann::json j;
    j["naab_version"] = naab_version;
    j["platform"] = platform;
    j["runtimes"] = nlohmann::json::array();
    for (const auto& e : runtimes) {
        nlohmann::json entry;
        entry["language"] = e.language;
        entry["runtime_version"] = e.runtime_version;
        entry["binary_path"] = e.binary_path;
        entry["timestamp"] = e.timestamp;
        j["runtimes"].push_back(entry);
    }

    std::ofstream ofs(path);
    if (ofs.is_open()) {
        ofs << j.dump(2) << "\n";
    }
}

// ============================================================================
// Lockfile::update
// ============================================================================

void Lockfile::update(const std::string& language,
                      const std::string& version,
                      const std::string& binary_path) {
    for (auto& e : runtimes) {
        if (e.language == language) {
            e.runtime_version = version;
            if (!binary_path.empty()) e.binary_path = binary_path;
            // Keep original timestamp — only update version
            return;
        }
    }
    // New entry
    LockfileEntry e;
    e.language = language;
    e.runtime_version = version;
    e.binary_path = binary_path;
    e.timestamp = currentTimestamp();
    runtimes.push_back(e);
}

// ============================================================================
// Lockfile::hasEntry / getEntry
// ============================================================================

bool Lockfile::hasEntry(const std::string& language) const {
    for (const auto& e : runtimes) {
        if (e.language == language) return true;
    }
    return false;
}

const LockfileEntry* Lockfile::getEntry(const std::string& language) const {
    for (const auto& e : runtimes) {
        if (e.language == language) return &e;
    }
    return nullptr;
}

// ============================================================================
// Lockfile::checkDrift
// ============================================================================

std::vector<std::string> Lockfile::checkDrift(
    const std::unordered_map<std::string, std::string>& observed) const
{
    std::vector<std::string> drifts;
    for (const auto& e : runtimes) {
        auto it = observed.find(e.language);
        if (it == observed.end()) continue;
        if (it->second != e.runtime_version) {
            drifts.push_back(
                "Runtime drift for " + e.language + ": "
                "locked=" + e.runtime_version + ", "
                "observed=" + it->second);
        }
    }
    return drifts;
}

// ============================================================================
// discoverLockfilePath
// ============================================================================

std::string discoverLockfilePath(const std::string& start_dir) {
    std::filesystem::path dir(start_dir);
    for (int depth = 0; depth < 10; ++depth) {
        std::filesystem::path candidate = dir / "govern.json";
        if (std::filesystem::exists(candidate)) {
            return (dir / ".naab" / "naab.lock").string();
        }
        std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return "";
}

} // namespace naab
